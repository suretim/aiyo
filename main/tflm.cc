#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

//#include "model.h"          //aa_bb_model_f32_lite  84716

#include <esp_log.h>
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "esp_nn.h"

#include "tflm.h"

#define INPUT_IMAGE_WIDTH       320
#define INPUT_IMAGE_HEIGHT      240
#define MODEL_INPUT_CHANNELS    3
#define NUM_CLASSES             2
#define THRESHOLD               0.5

static const char *TAG = "tflm";
 
namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;

typedef struct 
{
    int         width;
    int         height;
    uint8_t     *data;
} fb_data_t;


static uint8_t *tensor_arena = nullptr;
} 

#define INPUT_IMAGE_WIDTH       320
#define INPUT_IMAGE_HEIGHT      240
#define MODEL_INPUT_CHANNELS    3
#define NUM_CLASSES             2
#define THRESHOLD               0.5
const unsigned int MODEL_INPUT_WIDTH = 96; //224 160 96;
const unsigned int MODEL_INPUT_HEIGHT = 96;   
const float detect_threshold = 0.08;
const float conf_threshold = 0.1f;
// extern const unsigned char  g_model[];
// extern const unsigned int   MODEL_INPUT_WIDTH;
// extern const unsigned int   MODEL_INPUT_HEIGHT;

extern const unsigned char _binary_best_float32_tflite_start[] asm("_binary_micro_float32_tflite_start");
extern const unsigned char _binary_best_float32_tflite_end[]   asm("_binary_micro_float32_tflite_end");
const size_t   esp32_best_float32_tflite_len=_binary_best_float32_tflite_end-_binary_best_float32_tflite_start;

// extern const unsigned char _binary_best_float16_tflite_start[] asm("_binary_best_float16_tflite_start");
// extern const unsigned char _binary_best_float16_tflite_end[]   asm("_binary_best_float16_tflite_end");
// const size_t   esp32_best_float16_tflite_len=_binary_best_float16_tflite_end-_binary_best_float16_tflite_start;
 
float health_q[7]={0};

float  get_health(int idx)
{
    return health_q[idx];
}
void tflite_init(void) 
{
    unsigned int kTensorArenaSize;
    for(int i=0;i<7;i++)
    {
        health_q[i]=0.0f;
    }
    model = tflite::GetModel(_binary_best_float32_tflite_start);
    
    
    //model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) 
    {
        MicroPrintf("Model provided is schema version %d not equal to supported "
                    "version %d.", model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    if(     MODEL_INPUT_WIDTH == 224) kTensorArenaSize = 3 * 1024 * 1024;
    else if(MODEL_INPUT_WIDTH == 160) kTensorArenaSize = 3 * 1024 * 1024 / 2;
    else                              kTensorArenaSize = 3 * 1024 * 1024 / 4;

    kTensorArenaSize=1140000;

    if (tensor_arena == NULL) 
    {        
        tensor_arena = (uint8_t *) heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (tensor_arena == NULL) {
        printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
        return;
    }
    static tflite::MicroMutableOpResolver<20> resolver;
    resolver.AddConv2D();
    resolver.AddSoftmax();
    resolver.AddConcatenation();
    resolver.AddStridedSlice();
    resolver.AddReshape();
    resolver.AddTranspose();
    resolver.AddPad();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddSub(); 

    resolver.AddAveragePool2D();
    resolver.AddFullyConnected();
    resolver.AddMean();
    resolver.AddShape();
    resolver.AddPack();

    resolver.AddMaxPool2D();
    resolver.AddResizeNearestNeighbor();
    resolver.AddLogistic(); 

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        MicroPrintf("AllocateTensors() failed");
        return;
    }

    input = interpreter->input(0);
    printf("Input shape: [%d, %d, %d, %d]\n", 
        input->dims->data[0], input->dims->data[1], 
        input->dims->data[2], input->dims->data[3]);
    printf("Input type: %s\n", 
        (input->type == kTfLiteInt8) ? "INT8" : "FLOAT32");
    if (input->quantization.type == kTfLiteAffineQuantization) {
        float scale = input->params.scale;
        int zero_point = input->params.zero_point;
        printf("Quantization: scale=%.6f, zero_point=%d\n", scale, zero_point);
    }

    output = interpreter->output(0);

    printf("output shape: [%d, %d, %d, %d]\n", 
        output->dims->data[0], output->dims->data[1], 
        output->dims->data[2], output->dims->data[3]);
    printf("output type: %s\n", 
        (output->type == kTfLiteInt8) ? "INT8" : "FLOAT32");
    if (output->quantization.type == kTfLiteAffineQuantization) {
        float scale = output->params.scale;
        int zero_point = output->params.zero_point;
        printf("Quantization: scale=%.6f, zero_point=%d\n", scale, zero_point);
    }

//Input shape: [1, 160, 160, 3]
//Input type: FLOAT32
//output shape: [1, 6, 525, -34594]
//output type: FLOAT32    
}

//resize_image(image_data, INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT, 
//        resized_image, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT, MODEL_INPUT_CHANNELS);
    
void resize_image(const uint8_t *src, int src_width, int src_height, 
                  uint8_t *dst, int dst_width, int dst_height, int channels) {
    float x_ratio = (float)src_width / dst_width;
    float y_ratio = (float)src_height / dst_height;

    for (int y = 0; y < dst_height; y++) {
        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);
            // BGR → RGB 转换
            dst[(y * dst_width + x) * 3 + 0] = src[(src_y * src_width + src_x) * 3 + 2]; // R
            dst[(y * dst_width + x) * 3 + 1] = src[(src_y * src_width + src_x) * 3 + 1]; // G
            dst[(y * dst_width + x) * 3 + 2] = src[(src_y * src_width + src_x) * 3 + 0]; // B 
        }
    }
}

void post_process(uint8_t *image_data);

unsigned int run_inference(uint8_t *image_data) 
{
    unsigned int i;
    if(input == NULL) return 1;

    int64_t start_time = esp_timer_get_time();

    uint8_t *resized_image = (uint8_t *)heap_caps_malloc(MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT * MODEL_INPUT_CHANNELS,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(resized_image == NULL)
    {
        ESP_LOGE(TAG, "heap_caps_malloc failed");
        return 1;
    }

    resize_image(image_data, INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT, 
                resized_image, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT, MODEL_INPUT_CHANNELS);
    
    for (i = 0; i < MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT * MODEL_INPUT_CHANNELS; i++) {
        input->data.f[i] = resized_image[i] / 255.0f;
    }    

    free(resized_image);

    if (interpreter->Invoke() != kTfLiteOk) {        
        ESP_LOGE(TAG, "Invoke failed");
        return 1;
    }

    post_process(image_data);
    //unsigned int len = output->dims->data[2];
    //for(i = 0; i < len; i++)
    //{
    //    printf("%04d,%.3f %.3f %.3f %.3f %.3f %.3f\r\n", i, output->data.f[i], output->data.f[i+ len * 1],
    //        output->data.f[i + len * 2],output->data.f[i + len * 3],output->data.f[i + len * 4],output->data.f[i + len * 5]
    //    );
    //}

    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Inference time: %.2f ms", (end_time - start_time) / 1000.0); //150ms

    return 0;
}

struct st_box 
{
    unsigned short   x1, x2;            // 左上角+右下角坐标
    unsigned char    y1, y2;
    unsigned char    score;              // 置信度
    unsigned char    class_id;           // 类别ID
};

struct st_box_arr
{
    struct st_box*  box;               // 指向边界框数组的指针
    size_t          cnt;               // 当前存储的框数量
    size_t          len;               // 数组容量
};


void fb_gfx_fillRect(fb_data_t *fb, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    int32_t line_step = (fb->width - w) * 3;
    uint8_t *data = fb->data + ((x + (y * fb->width)) * 3);
    uint8_t c0 = color >> 16;
    uint8_t c1 = color >> 8;
    uint8_t c2 = color;
    for (int i=0; i<h; i++){
        for (int j=0; j<w; j++){
            data[0] = c0;
            data[1] = c1;
            data[2] = c2;
            data+=3;
        }
        data += line_step;
    }
}

void fb_gfx_drawFastHLine(fb_data_t *fb, int32_t x, int32_t y, int32_t w, uint32_t color)
{
    fb_gfx_fillRect(fb, x, y, w, 1, color);
}

void fb_gfx_drawFastVLine(fb_data_t *fb, int32_t x, int32_t y, int32_t h, uint32_t color)
{
    fb_gfx_fillRect(fb, x, y, 1, h, color);
}

void post_process(uint8_t *image_data)
{
    struct st_box_arr   arr = {0};

    arr.box = (struct st_box*)heap_caps_malloc(output->dims->data[2] * 2 * sizeof(struct st_box),  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(arr.box == NULL)
    {
        printf("box_arr_malloc error!\r\n");
        return;
    }
    arr.cnt = 0;
    arr.len = output->dims->data[2] * 2;


    float conf_1, conf_0,  x, y, w, h, x1, x2, y1, y2, conf_threshold = 0.1f, stride = 8.0f;
    unsigned int i, len, stride8_len, stride16_len, grid_8, grid_16, grid_32, grid, grid_i, grid_x, grid_y;

    grid_8  = MODEL_INPUT_WIDTH / 8;                 //20*20
    grid_16 = MODEL_INPUT_WIDTH / 16;                //10*10
    grid_32 = MODEL_INPUT_WIDTH / 32;                //5*5

    stride8_len  = grid_8 * grid_8;
    stride16_len = stride8_len + grid_16 * grid_16;

    len = output->dims->data[2];
    for (i = 0; i < len; i++) 
    {
        conf_0 = output->data.f[5 * len + i];
        conf_1 = output->data.f[4 * len + i];
        if((conf_0 >= conf_threshold)||(conf_1 >= conf_threshold))
        {
            if(     i < stride8_len ) {grid_i = i;                grid = grid_8;}
            else if(i < stride16_len) {grid_i = i - stride8_len;  grid = grid_16;}
            else                      {grid_i = i - stride16_len; grid = grid_32;}
            grid_x = grid_i % grid;
            grid_y = grid_i / grid;

            x = (output->data.f[0 * len + i] + grid_x) * stride * INPUT_IMAGE_WIDTH / MODEL_INPUT_WIDTH;
            y = (output->data.f[1 * len + i] + grid_y) * stride * INPUT_IMAGE_HEIGHT / MODEL_INPUT_HEIGHT;
            float sigmoid_w = 1.0f / (1.0f + expf(-output->data.f[2 * len + i]));
            float sigmoid_h = 1.0f / (1.0f + expf(-output->data.f[3 * len + i]));
            w = powf(sigmoid_w * 2, 2) * stride * INPUT_IMAGE_WIDTH / MODEL_INPUT_WIDTH;
            h = powf(sigmoid_h * 2, 2) * stride * INPUT_IMAGE_HEIGHT / MODEL_INPUT_HEIGHT;            
            x1 = x - w / 2;
            x2 = x + w / 2;
            y1 = y - h / 2;
            y2 = y + h / 2;

            if( (x1 >= 0)&&(x1 < INPUT_IMAGE_WIDTH)&&(x2 >= 0)&&(x2 < INPUT_IMAGE_WIDTH) &&
                (y1 >= 0)&&(y1 < INPUT_IMAGE_HEIGHT)&&(y2 >= 0)&&(y2 < INPUT_IMAGE_HEIGHT))
            {
                if(conf_0 >= conf_threshold)
                {
                    arr.box[arr.cnt].x1 = x1; 
                    arr.box[arr.cnt].x2 = x2; 
                    arr.box[arr.cnt].y1 = y1; 
                    arr.box[arr.cnt].y2 = y2; 
                    arr.box[arr.cnt].score = conf_0 * 100;
                    arr.box[arr.cnt].class_id = 0; 
                    arr.cnt++;
                }
                if(conf_1 >= conf_threshold)
                {
                    arr.box[arr.cnt].x1 = x1; 
                    arr.box[arr.cnt].x2 = x2; 
                    arr.box[arr.cnt].y1 = y1; 
                    arr.box[arr.cnt].y2 = y2; 
                    arr.box[arr.cnt].score = conf_1 * 100;
                    arr.box[arr.cnt].class_id = 1; 
                    arr.cnt++;
                }
            }
        }
    }

    unsigned int aa_x1 = INPUT_IMAGE_WIDTH, aa_x2 = 0, aa_y1 = INPUT_IMAGE_HEIGHT, aa_y2 = 0;
    unsigned int bb_x1 = INPUT_IMAGE_WIDTH, bb_x2 = 0, bb_y1 = INPUT_IMAGE_HEIGHT, bb_y2 = 0;
    
    for (i = 0; i < arr.cnt; i++) 
    {
        if(arr.box[i].class_id == 0)
        {
            if(arr.box[i].x1 < aa_x1) aa_x1 = arr.box[i].x1;
            if(arr.box[i].y1 < aa_y1) aa_y1 = arr.box[i].y1;
            if(arr.box[i].x2 > aa_x2) aa_x2 = arr.box[i].x2;
            if(arr.box[i].y2 > aa_y2) aa_y2 = arr.box[i].y2;
        }
        else
        {
            if(arr.box[i].x1 < bb_x1) bb_x1 = arr.box[i].x1;
            if(arr.box[i].y1 < bb_y1) bb_y1 = arr.box[i].y1;
            if(arr.box[i].x2 > bb_x2) bb_x2 = arr.box[i].x2;
            if(arr.box[i].y2 > bb_y2) bb_y2 = arr.box[i].y2;
        }
    }

    free(arr.box); 
    arr.box = NULL;

    fb_data_t fb_data;
    unsigned int color, ww, hh;

    fb_data.width = INPUT_IMAGE_WIDTH;
    fb_data.height = INPUT_IMAGE_HEIGHT;
    fb_data.data = image_data;

    if(aa_x1 != INPUT_IMAGE_WIDTH)
    {
        ww = aa_x2 - aa_x1;
        hh = aa_y2 - aa_y1;
        color = 0x0000ff00;
        fb_gfx_drawFastHLine(&fb_data, aa_x1, aa_y1, ww, color);
        fb_gfx_drawFastHLine(&fb_data, aa_x1, aa_y1 + hh - 1, ww, color);
        fb_gfx_drawFastVLine(&fb_data, aa_x1, aa_y1, hh, color);
        fb_gfx_drawFastVLine(&fb_data, aa_x1 + ww - 1, aa_y1, hh, color);
    }

    if(bb_x1 != INPUT_IMAGE_WIDTH)
    {
        ww = bb_x2 - bb_x1;
        hh = bb_y2 - bb_y1;
        color = 0x00ff0000;
        fb_gfx_drawFastHLine(&fb_data, bb_x1, bb_y1, ww, color);
        fb_gfx_drawFastHLine(&fb_data, bb_x1, bb_y1 + hh - 1, ww, color);
        fb_gfx_drawFastVLine(&fb_data, bb_x1, bb_y1, hh, color);
        fb_gfx_drawFastVLine(&fb_data, bb_x1 + ww - 1, bb_y1, hh, color);
    }    
}