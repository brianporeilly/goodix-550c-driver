/* Minimal libfprint open test: open the first device (our 550c), report, close.
 * Exercises the full open SSM — variant detect, TLS-PSK handshake, config. */
#include <libfprint/fprint.h>
#include <stdio.h>

int
main (void)
{
  g_autoptr(FpContext) ctx = fp_context_new ();
  GPtrArray *devices = fp_context_get_devices (ctx);
  g_autoptr(GError) error = NULL;

  if (devices == NULL || devices->len == 0)
    {
      printf ("NO DEVICES FOUND\n");
      return 2;
    }

  FpDevice *dev = g_ptr_array_index (devices, 0);
  printf ("Device: %s (%s)\n", fp_device_get_name (dev),
          fp_device_get_device_id (dev));

  if (!fp_device_open_sync (dev, NULL, &error))
    {
      printf ("OPEN FAILED: %s\n", error->message);
      return 1;
    }

  printf ("OPEN OK\n");
  fp_device_close_sync (dev, NULL, NULL);
  printf ("CLOSED\n");
  return 0;
}
