#ifndef __tflm_h__
#define __tflm_h__

#ifdef __cplusplus
extern "C" {
#endif

extern void tflite_init(void);

extern unsigned int run_inference(unsigned char *image_data);

#ifdef __cplusplus
}
#endif

#endif
