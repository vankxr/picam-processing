#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <time.h>
#include <libv4l2.h>
#include <QPULib.h>

#define FRAME_WIDTH     640
#define FRAME_HEIGHT    480
#define IIR_COEF_LOG2   1 // log2(coef) i.e Coef = 4, set to 2

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer
{
    void   *start;
    size_t length;
};

static void xioctl(int fh, int request, void *arg)
{
    int r;

    do
    {
        r = v4l2_ioctl(fh, request, arg);
    }
    while (r == -1 && ((errno == EINTR) || (errno == EAGAIN)));

    if (r == -1)
    {
        fprintf(stderr, "error %d, %s\n", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void process_frame(Int n, Ptr<Int> curr, Ptr<Int> prev, Ptr<Int> filter)
{
    Int inc = numQPUs() << 4;

    Ptr<Int> x = curr + index() + (me() << 4);
    Ptr<Int> y = prev + index() + (me() << 4);
    Ptr<Int> z = filter + index() + (me() << 4);

    gather(x); // Fetch first 16 pixels
    gather(y);
    gather(z);

    Int icurr;
    Int iprev;
    Int ifilter;

    For(Int i = 0, i < n, i = i + inc)
        receive(icurr); // Move 16 pixels from FIFO to the variables
        receive(iprev);
        receive(ifilter);

        gather(x + inc); // Prefetch next 16 pixels
        gather(y + inc);
        gather(z + inc);

        Int diff;

        Where(icurr == iprev)
            diff = icurr - iprev;
        End
        Where(icurr > iprev)
            diff = icurr - iprev;
        End
        Where(icurr < iprev)
            diff = iprev - icurr;
        End

        store(icurr, y);

        Int filtered = (ifilter * ((1 << IIR_COEF_LOG2) - 1) + diff) >> IIR_COEF_LOG2;

        store(filtered, z);
        store(filtered, x);

        x = x + inc;
        y = y + inc;
        z = z + inc;
    End

    receive(icurr); // Discard prefetched frames from final iteration
    receive(iprev);
    receive(ifilter);
}
void process_frame_no_filter(Int n, Ptr<Int> curr, Ptr<Int> prev, Ptr<Int> filter)
{
    Int inc = numQPUs() << 4;

    Ptr<Int> x = curr + index() + (me() << 4);
    Ptr<Int> y = prev + index() + (me() << 4);
    Ptr<Int> z = filter + index() + (me() << 4);

    gather(x); // Fetch next 16 pixels
    gather(y);

    Int icurr;
    Int iprev;

    For(Int i = 0, i < n, i = i + inc)
        gather(x + inc); // Prefetch next 16 pixels
        gather(y + inc);

        receive(icurr); // Move 16 pixels from FIFO to the variables
        receive(iprev);

        Int diff;

        Where(icurr == iprev)
            diff = 0;
        End
        Where(icurr > iprev)
            diff = icurr - iprev;
        End
        Where(icurr < iprev)
            diff = iprev - icurr;
        End

        store(icurr, y);

        store(diff, x);
        store(diff, z);

        x = x + inc;
        y = y + inc;
        z = z + inc;
    End

    receive(icurr); // Discard prefetched frames from final iteration
    receive(iprev);
}

int main(int argc, char **argv)
{
    struct v4l2_format              fmt;
    struct v4l2_buffer              buf;
    struct v4l2_requestbuffers      req;
    enum v4l2_buf_type              type;
    fd_set                          fds;
    struct timeval                  tv;
    int                             r, fd = -1;
    unsigned int                    i, n_buffers;
    const char                      *dev_name = "/dev/video0";
    char                            out_name[256];
    FILE                            *fout;
    struct buffer                   *buffers;
    SharedArray<int>                current(FRAME_WIDTH * FRAME_HEIGHT);
    SharedArray<int>                prev(FRAME_WIDTH * FRAME_HEIGHT);
    SharedArray<int>                filter(FRAME_WIDTH * FRAME_HEIGHT);
    int                             n = FRAME_WIDTH * FRAME_HEIGHT;

    // Compile QPU kernels
    auto qpu_process_frame = compile(process_frame);
    auto qpu_process_frame_no_filter = compile(process_frame_no_filter);

    // Assign 2 QPUs to each kernel
    qpu_process_frame.setNumQPUs(2);
    qpu_process_frame_no_filter.setNumQPUs(2);

    // Open camera device
    fd = v4l2_open(dev_name, O_RDWR | O_NONBLOCK, 0);

    if (fd < 0)
    {
        perror("Cannot open device");
        exit(EXIT_FAILURE);
    }

    // Initialize capture format structure
    CLEAR(fmt);
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = FRAME_WIDTH;
    fmt.fmt.pix.height      = FRAME_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24; // Camera does not support GREY, will convert later
    fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

    // Set capture format
    xioctl(fd, VIDIOC_S_FMT, &fmt);

    // Sanity checks
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24)
    {
        printf("Libv4l didn't accept RGB24 format. Can't proceed.\n");
        exit(EXIT_FAILURE);
    }

    if ((fmt.fmt.pix.width != FRAME_WIDTH) || (fmt.fmt.pix.height != FRAME_HEIGHT))
        printf("Warning: driver is sending image at %dx%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // Initialize buffer configuration
    CLEAR(req);
    req.count   = 2;
    req.type    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory  = V4L2_MEMORY_MMAP;

    // Request buffers
    xioctl(fd, VIDIOC_REQBUFS, &req);

    // Allocate shared buffers
    buffers = (struct buffer *)calloc(req.count, sizeof(struct buffer));

    for (n_buffers = 0; n_buffers < req.count; n_buffers++)
    {
        // Initialize buffers
        CLEAR(buf);
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = n_buffers;

        xioctl(fd, VIDIOC_QUERYBUF, &buf);

        // Request memory from v4l2 library
        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = v4l2_mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
        {
            perror("mmap");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < n_buffers; i++)
    {
        // Initialize buffers again in case the previous request changed something
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        // Queue buffer
        xioctl(fd, VIDIOC_QBUF, &buf);
    }

    // Zero buffers chared with QPUs
    for(int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
    {
        prev[i] = 0;
        filter[i] = 0;
    }

    // Turn video streaming on
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMON, &type);

    for (i = 0; i < 20; i++)
    {
        // Wait for a frame to be ready
        do
        {
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);
        }
        while ((r == -1 && (errno = EINTR)));

        if (r == -1)
        {
            perror("select");
            return errno;
        }

        // Re-configure buffer
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // Remove buffer from que queue
        xioctl(fd, VIDIOC_DQBUF, &buf);

        // Convert RGB to greyscale
        int k = 0;
        for(int j = 0; j < buf.bytesused; j += 3)
        {
            uint8_t red = ((uint8_t *)buffers[buf.index].start)[j + 0];
            uint8_t green = ((uint8_t *)buffers[buf.index].start)[j + 1];
            uint8_t blue = ((uint8_t *)buffers[buf.index].start)[j + 2];

            double grey = red * 0.2989 + green * 0.5870 + blue * 0.1140;

            current[k] = (int)grey;

            k++;
        }

        // Initialize clock to measure performance
        clock_t t;
        t = clock();

        if(!i)
            qpu_process_frame_no_filter(n, &current, &prev, &filter);
        else
            qpu_process_frame(n, &current, &prev, &filter);

        t = clock() - t;
        double time_taken = ((double)t)/CLOCKS_PER_SEC; // in seconds

        printf("took %f seconds to process frame\r\n", time_taken);

        // Assign all RGB channels with the same greyscale value to produce a greyscale image
        k = 0;
        for(int j = 0; j < buf.bytesused; j += 3)
        {
            ((uint8_t *)buffers[buf.index].start)[j + 0] = (uint8_t)current[k];
            ((uint8_t *)buffers[buf.index].start)[j + 1] = (uint8_t)current[k];
            ((uint8_t *)buffers[buf.index].start)[j + 2] = (uint8_t)current[k];

            k++;
        }

        // Open output file
        sprintf(out_name, "img%03d.ppm", i);
        fout = fopen(out_name, "w");

        printf("writting %s from buffer %u\r\n", out_name, buf.index);

        if (!fout)
        {
            perror("Cannot open image");
            exit(EXIT_FAILURE);
        }

        // Write PPM header and image data
        fprintf(fout, "P6\n%d %d 255\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
        fwrite(buffers[buf.index].start, buf.bytesused, 1, fout);
        fclose(fout);

        // Re-add buffer to queue
        xioctl(fd, VIDIOC_QBUF, &buf);
    }

    // Turn video streaming off
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);

    // Free shared buffers
    for (i = 0; i < n_buffers; ++i)
        v4l2_munmap(buffers[i].start, buffers[i].length);

    v4l2_close(fd);

    return 0;
}
