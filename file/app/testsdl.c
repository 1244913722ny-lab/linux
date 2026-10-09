#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    perror("open");
    return 1;
  }
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  ioctl(fd, FBIOGET_VSCREENINFO, &v);
  ioctl(fd, FBIOGET_FSCREENINFO, &f);
  printf("bpp=%u xres=%u yres=%u smem_len=%u\n", v.bits_per_pixel,
         v.xres_virtual, v.yres_virtual, f.smem_len);
  void *fb = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  if (v.bits_per_pixel == 16) {
    unsigned short *p = fb;
    unsigned n = f.smem_len / 2;
    for (unsigned i = 0; i < n; i++)
      p[i] = 0xF800; /* RGB565 红 */
  } else if (v.bits_per_pixel == 32) {
    unsigned *p = fb;
    unsigned n = f.smem_len / 4;
    for (unsigned i = 0; i < n; i++)
      p[i] = 0x00FF0000; /* XRGB8888 红 */
  }
  printf("readback[0]=0x%X  done\n", (v.bits_per_pixel == 16)
                                         ? ((unsigned short *)fb)[0]
                                         : ((unsigned *)fb)[0]);
  printf(">>> SDL 现在应该是红色 <<<\n");
  printf("smem_start=0x%lX\n", (unsigned long)f.smem_start);
  sleep(2);
  return 0;
}
