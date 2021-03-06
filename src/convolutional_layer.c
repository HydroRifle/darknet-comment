#include "convolutional_layer.h"
#include "utils.h"
#include "batchnorm_layer.h"
#include "im2col.h"
#include "col2im.h"
#include "blas.h"
#include "gemm.h"
#include <stdio.h>
#include <time.h>

#ifdef AI2
#include "xnor_layer.h"
#endif

void swap_binary(convolutional_layer *l)
{
    float *swap = l->weights;
    l->weights = l->binary_weights;
    l->binary_weights = swap;

#ifdef GPU
    swap = l->weights_gpu;
    l->weights_gpu = l->binary_weights_gpu;
    l->binary_weights_gpu = swap;
#endif
}

void binarize_weights(float *weights, int n, int size, float *binary)
{
    int i, f;
    for(f = 0; f < n; ++f){
        float mean = 0;
        for(i = 0; i < size; ++i){
            mean += fabs(weights[f*size + i]);
        }
        mean = mean / size;
        for(i = 0; i < size; ++i){
            binary[f*size + i] = (weights[f*size + i] > 0) ? mean : -mean;
        }
    }
}

void binarize_cpu(float *input, int n, float *binary)
{
    int i;
    for(i = 0; i < n; ++i){
        binary[i] = (input[i] > 0) ? 1 : -1;
    }
}

void binarize_input(float *input, int n, int size, float *binary)
{
    int i, s;
    for(s = 0; s < size; ++s){
        float mean = 0;
        for(i = 0; i < n; ++i){
            mean += fabs(input[i*size + s]);
        }
        mean = mean / n;
        for(i = 0; i < n; ++i){
            binary[i*size + s] = (input[i*size + s] > 0) ? mean : -mean;
        }
    }
}

/*
输入：卷积层 l
功能：计算卷积层输出的 height 大小
输出：height 大小
*/
int convolutional_out_height(convolutional_layer l)
{
    return (l.h + 2*l.pad - l.size) / l.stride + 1;
}

/*
输入：卷积层 l
功能：计算卷积层输出的 width 大小
输出：widt 大小
*/
int convolutional_out_width(convolutional_layer l)
{
    return (l.w + 2*l.pad - l.size) / l.stride + 1;
}

image get_convolutional_image(convolutional_layer l)
{
    return float_to_image(l.out_w,l.out_h,l.out_c,l.output);
}

image get_convolutional_delta(convolutional_layer l)
{
    return float_to_image(l.out_w,l.out_h,l.out_c,l.delta);
}

static size_t get_workspace_size(layer l){
#ifdef CUDNN
    if(gpu_index >= 0){
        size_t most = 0;
        size_t s = 0;
        cudnnGetConvolutionForwardWorkspaceSize(cudnn_handle(),
                l.srcTensorDesc,
                l.weightDesc,
                l.convDesc,
                l.dstTensorDesc,
                l.fw_algo,
                &s);
        if (s > most) most = s;
        cudnnGetConvolutionBackwardFilterWorkspaceSize(cudnn_handle(),
                l.srcTensorDesc,
                l.ddstTensorDesc,
                l.convDesc,
                l.dweightDesc,
                l.bf_algo,
                &s);
        if (s > most) most = s;
        cudnnGetConvolutionBackwardDataWorkspaceSize(cudnn_handle(),
                l.weightDesc,
                l.ddstTensorDesc,
                l.convDesc,
                l.dsrcTensorDesc,
                l.bd_algo,
                &s);
        if (s > most) most = s;
        return most;
    }
#endif
    return (size_t)l.out_h*l.out_w*l.size*l.size*l.c/l.groups*sizeof(float);
}

#ifdef GPU
#ifdef CUDNN
void cudnn_convolutional_setup(layer *l)
{
    cudnnSetTensor4dDescriptor(l->dsrcTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->c, l->h, l->w); 
    cudnnSetTensor4dDescriptor(l->ddstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w); 

    cudnnSetTensor4dDescriptor(l->srcTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->c, l->h, l->w); 
    cudnnSetTensor4dDescriptor(l->dstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w); 
    cudnnSetTensor4dDescriptor(l->normTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, 1, l->out_c, 1, 1); 

    cudnnSetFilter4dDescriptor(l->dweightDesc, CUDNN_DATA_FLOAT, CUDNN_TENSOR_NCHW, l->n, l->c/l->groups, l->size, l->size); 
    cudnnSetFilter4dDescriptor(l->weightDesc, CUDNN_DATA_FLOAT, CUDNN_TENSOR_NCHW, l->n, l->c/l->groups, l->size, l->size); 
    #if CUDNN_MAJOR >= 6
    cudnnSetConvolution2dDescriptor(l->convDesc, l->pad, l->pad, l->stride, l->stride, 1, 1, CUDNN_CROSS_CORRELATION, CUDNN_DATA_FLOAT);
    #else
    cudnnSetConvolution2dDescriptor(l->convDesc, l->pad, l->pad, l->stride, l->stride, 1, 1, CUDNN_CROSS_CORRELATION);
    #endif

    #if CUDNN_MAJOR >= 7
    cudnnSetConvolutionGroupCount(l->convDesc, l->groups);
    #else
    if(l->groups > 1){
        error("CUDNN < 7 doesn't support groups, please upgrade!");
    }
    #endif

    cudnnGetConvolutionForwardAlgorithm(cudnn_handle(),
            l->srcTensorDesc,
            l->weightDesc,
            l->convDesc,
            l->dstTensorDesc,
            CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
            2000000000,
            &l->fw_algo);
    cudnnGetConvolutionBackwardDataAlgorithm(cudnn_handle(),
            l->weightDesc,
            l->ddstTensorDesc,
            l->convDesc,
            l->dsrcTensorDesc,
            CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
            2000000000,
            &l->bd_algo);
    cudnnGetConvolutionBackwardFilterAlgorithm(cudnn_handle(),
            l->srcTensorDesc,
            l->ddstTensorDesc,
            l->convDesc,
            l->dweightDesc,
            CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
            2000000000,
            &l->bf_algo);
}
#endif
#endif

/*
输入：
    n  filters的数量
    c  输入数据的通道数
功能：对卷阶层各个参数进行分配内存或者初始化，并计算输入输出大小
返回：分配内存或者初始化后的卷阶层layer
*/
convolutional_layer make_convolutional_layer(int batch, int h, int w, int c, int n, int groups, int size, int stride, int padding, ACTIVATION activation, int batch_normalize, int binary, int xnor, int adam)
{
    int i;
    // 初始化结构体变量，这里的是局部变量；calloc sizeof(convolutional_layer)分配在堆上
    convolutional_layer l = {0};
    l.type = CONVOLUTIONAL;

    l.groups = groups;
    l.h = h;  // 输入数据的高
    l.w = w;   // 输入数据的宽
    l.c = c;  // 输入数据的通道数
    l.n = n;  // filters的数量
    l.binary = binary;
    l.xnor = xnor;
    l.batch = batch;
    l.stride = stride;
    l.size = size;
    l.pad = padding;
    l.batch_normalize = batch_normalize;

    /*
    groups:意思是将对应的输入通道与输出通道数进行分组，比如输入数据大小为90x100x100x32 90是数据批大小 100x100是图像数据shape，32是通道数，要经过一个3x3x48的卷积，
    group默认是1，就是全连接的卷积层，
    如果group是2，那么对应要将输入的32个通道分成2个16的通道，将输出的48个通道分成2个24的通道。对输出的2个24的通道，第一个24通道与输入的第一个16通道进行全卷积，第二个24通道与输入的第二个16通道进行全卷积。
    极端情况下，输入输出通道数相同，比如为24，group大小也为24，那么每个输出卷积核，只与输入的对应的通道进行卷积。

    groups将输入的channel分割成groups份，对应的卷积核channel也分割成groups份，c/groups*n*size*size得到总的权重数量
    */
    l.weights = calloc(c/groups*n*size*size, sizeof(float));  // 为权重分配内存
    l.weight_updates = calloc(c/groups*n*size*size, sizeof(float));

    l.biases = calloc(n, sizeof(float));  // 为偏置分配内存
    l.bias_updates = calloc(n, sizeof(float));

    l.nweights = c/groups*n*size*size;  // 总权重数量
    l.nbiases = n; // 总偏置数量

    // float scale = 1./sqrt(size*size*c);
    float scale = sqrt(2./(size*size*c/l.groups));
    //printf("convscale %f\n", scale);
    //scale = .02;
    //for(i = 0; i < c*n*size*size; ++i) l.weights[i] = scale*rand_uniform(-1, 1);
    for(i = 0; i < l.nweights; ++i) l.weights[i] = scale*rand_normal();  // 权重初始化
    int out_w = convolutional_out_width(l);
    int out_h = convolutional_out_height(l);
    l.out_h = out_h;
    l.out_w = out_w;
    l.out_c = n;
    l.outputs = l.out_h * l.out_w * l.out_c; // 每次batch输出的大小
    l.inputs = l.w * l.h * l.c;  // 每次batch输入的大小

    l.output = calloc(l.batch*l.outputs, sizeof(float)); // 存放每组batch总输出
    l.delta  = calloc(l.batch*l.outputs, sizeof(float));  // 存放损失函数关于每batch个样本的 z 的梯度

    l.forward = forward_convolutional_layer;   // 结构体中函数的应用
    l.backward = backward_convolutional_layer;
    l.update = update_convolutional_layer;
    if(binary){
        l.binary_weights = calloc(l.nweights, sizeof(float));
        l.cweights = calloc(l.nweights, sizeof(char));
        l.scales = calloc(n, sizeof(float));
    }
    if(xnor){
        l.binary_weights = calloc(l.nweights, sizeof(float));
        l.binary_input = calloc(l.inputs*l.batch, sizeof(float));
    }

    if(batch_normalize){
        l.scales = calloc(n, sizeof(float));  // batchnorm 层中gamma参数初始化
        l.scale_updates = calloc(n, sizeof(float));
        for(i = 0; i < n; ++i){
            l.scales[i] = 1;
        }

        l.mean = calloc(n, sizeof(float));
        l.variance = calloc(n, sizeof(float));

        l.mean_delta = calloc(n, sizeof(float));
        l.variance_delta = calloc(n, sizeof(float));

        l.rolling_mean = calloc(n, sizeof(float));
        l.rolling_variance = calloc(n, sizeof(float));
        l.x = calloc(l.batch*l.outputs, sizeof(float));
        l.x_norm = calloc(l.batch*l.outputs, sizeof(float));
    }
    if(adam){
        l.m = calloc(l.nweights, sizeof(float));
        l.v = calloc(l.nweights, sizeof(float));
        l.bias_m = calloc(n, sizeof(float));
        l.scale_m = calloc(n, sizeof(float));
        l.bias_v = calloc(n, sizeof(float));
        l.scale_v = calloc(n, sizeof(float));
    }

#ifdef GPU
    l.forward_gpu = forward_convolutional_layer_gpu;
    l.backward_gpu = backward_convolutional_layer_gpu;
    l.update_gpu = update_convolutional_layer_gpu;

    if(gpu_index >= 0){
        if (adam) {
            l.m_gpu = cuda_make_array(l.m, l.nweights);
            l.v_gpu = cuda_make_array(l.v, l.nweights);
            l.bias_m_gpu = cuda_make_array(l.bias_m, n);
            l.bias_v_gpu = cuda_make_array(l.bias_v, n);
            l.scale_m_gpu = cuda_make_array(l.scale_m, n);
            l.scale_v_gpu = cuda_make_array(l.scale_v, n);
        }

        l.weights_gpu = cuda_make_array(l.weights, l.nweights);
        l.weight_updates_gpu = cuda_make_array(l.weight_updates, l.nweights);

        l.biases_gpu = cuda_make_array(l.biases, n);
        l.bias_updates_gpu = cuda_make_array(l.bias_updates, n);

        l.delta_gpu = cuda_make_array(l.delta, l.batch*out_h*out_w*n);
        l.output_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);

        if(binary){
            l.binary_weights_gpu = cuda_make_array(l.weights, l.nweights);
        }
        if(xnor){
            l.binary_weights_gpu = cuda_make_array(l.weights, l.nweights);
            l.binary_input_gpu = cuda_make_array(0, l.inputs*l.batch);
        }

        if(batch_normalize){
            l.mean_gpu = cuda_make_array(l.mean, n);
            l.variance_gpu = cuda_make_array(l.variance, n);

            l.rolling_mean_gpu = cuda_make_array(l.mean, n);
            l.rolling_variance_gpu = cuda_make_array(l.variance, n);

            l.mean_delta_gpu = cuda_make_array(l.mean, n);
            l.variance_delta_gpu = cuda_make_array(l.variance, n);

            l.scales_gpu = cuda_make_array(l.scales, n);
            l.scale_updates_gpu = cuda_make_array(l.scale_updates, n);

            l.x_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);
            l.x_norm_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);
        }
#ifdef CUDNN
        cudnnCreateTensorDescriptor(&l.normTensorDesc);
        cudnnCreateTensorDescriptor(&l.srcTensorDesc);
        cudnnCreateTensorDescriptor(&l.dstTensorDesc);
        cudnnCreateFilterDescriptor(&l.weightDesc);
        cudnnCreateTensorDescriptor(&l.dsrcTensorDesc);
        cudnnCreateTensorDescriptor(&l.ddstTensorDesc);
        cudnnCreateFilterDescriptor(&l.dweightDesc);
        cudnnCreateConvolutionDescriptor(&l.convDesc);
        cudnn_convolutional_setup(&l);
#endif
    }
#endif
    l.workspace_size = get_workspace_size(l);
    l.activation = activation;

    fprintf(stderr, "conv  %5d %2d x%2d /%2d  %4d x%4d x%4d   ->  %4d x%4d x%4d  %5.3f BFLOPs\n", n, size, size, stride, w, h, c, l.out_w, l.out_h, l.out_c, (2.0 * l.n * l.size*l.size*l.c/l.groups * l.out_h*l.out_w)/1000000000.);
    // 这里跟返回int一样，会将该值直接赋值。不能返回的是 &l
    return l;
}

void denormalize_convolutional_layer(convolutional_layer l)
{
    int i, j;
    for(i = 0; i < l.n; ++i){
        float scale = l.scales[i]/sqrt(l.rolling_variance[i] + .00001);
        for(j = 0; j < l.c/l.groups*l.size*l.size; ++j){
            l.weights[i*l.c/l.groups*l.size*l.size + j] *= scale;
        }
        l.biases[i] -= l.rolling_mean[i] * scale;
        l.scales[i] = 1;
        l.rolling_mean[i] = 0;
        l.rolling_variance[i] = 1;
    }
}

/*
void test_convolutional_layer()
{
    convolutional_layer l = make_convolutional_layer(1, 5, 5, 3, 2, 5, 2, 1, LEAKY, 1, 0, 0, 0);
    l.batch_normalize = 1;
    float data[] = {1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3};
    //net.input = data;
    //forward_convolutional_layer(l);
}
*/

void resize_convolutional_layer(convolutional_layer *l, int w, int h)
{
    l->w = w;
    l->h = h;
    int out_w = convolutional_out_width(*l);
    int out_h = convolutional_out_height(*l);

    l->out_w = out_w;
    l->out_h = out_h;

    l->outputs = l->out_h * l->out_w * l->out_c;
    l->inputs = l->w * l->h * l->c;

    l->output = realloc(l->output, l->batch*l->outputs*sizeof(float));
    l->delta  = realloc(l->delta,  l->batch*l->outputs*sizeof(float));
    if(l->batch_normalize){
        l->x = realloc(l->x, l->batch*l->outputs*sizeof(float));
        l->x_norm  = realloc(l->x_norm, l->batch*l->outputs*sizeof(float));
    }

#ifdef GPU
    cuda_free(l->delta_gpu);
    cuda_free(l->output_gpu);

    l->delta_gpu =  cuda_make_array(l->delta,  l->batch*l->outputs);
    l->output_gpu = cuda_make_array(l->output, l->batch*l->outputs);

    if(l->batch_normalize){
        cuda_free(l->x_gpu);
        cuda_free(l->x_norm_gpu);

        l->x_gpu = cuda_make_array(l->output, l->batch*l->outputs);
        l->x_norm_gpu = cuda_make_array(l->output, l->batch*l->outputs);
    }
#ifdef CUDNN
    cudnn_convolutional_setup(l);
#endif
#endif
    l->workspace_size = get_workspace_size(*l);
}

/*
输入：网络输出大小 output
     偏置大小    biases
     batch大小  batch
     卷积核的数目 n
     每次卷积输出的高和宽的乘积 size
功能：为网络的输出加上偏置，每一个卷积核对应一个偏置
输出：经过shift操作后的float *output
返回：无
*/
void add_bias(float *output, float *biases, int batch, int n, int size)
{
    int i,j,b;
    for(b = 0; b < batch; ++b){  // 对于每一个batch
        for(i = 0; i < n; ++i){  // 对于每一个卷积核
            for(j = 0; j < size; ++j){  //对于每一个输出
                output[(b*n + i)*size + j] += biases[i]; // 每一个卷积核对应一个偏置
            }
        }
    }
}

/*
输入: 网络输出 output
      系数大小    scales
      batch大小  batch
      卷积核的数目 n
      每次卷积输出的高和宽的乘积 size   对于CNN 等于输出的长和宽乘积，对于DNN 等于1，保证代码具有统一性
功能：为网络的输出乘上系数，每一个卷积核对应一个系数
输出：经过scale操作后的float *output
返回：无
*/
void scale_bias(float *output, float *scales, int batch, int n, int size)
{
    int i,j,b;
    for(b = 0; b < batch; ++b){ // 对于每一个batch
        for(i = 0; i < n; ++i){ // 对于每一个卷积核
            for(j = 0; j < size; ++j){ //对于每一个输出
                output[(b*n + i)*size + j] *= scales[i];  // 每一个卷积核或神经元均有一个系数，点乘
            }
        }
    }
}

/* 
输入：存放损失关于偏置的偏导数                                      *bias_updates
     当前层的敏感图                                               *delta
     batch的大小                                                  batch
     将卷积核分组后，每个卷积核的参数总数                           n
     每次输出的高和宽的乘积                                    size
功能：对损失函数关于每一个卷积核的偏置求导数，并放到 float *bias_updates中
输出：损失函数关于每一个卷积核偏置的导数 float *bias_updates
返回：无
*/
void backward_bias(float *bias_updates, float *delta, int batch, int n, int size)
{
    int i,b;
    for(b = 0; b < batch; ++b){  // 对于每一个batch
        for(i = 0; i < n; ++i){   // 对于每一个卷积核
            // 同一个batch之内，相同位置的特征图对应同一个偏置，这也可以理解为一个卷积核对应一个偏置，因此损失函数关于卷积核偏置的偏导数，等于同一个batch内相同位置 delta 的和
            bias_updates[i] += sum_array(delta+size*(i+b*n), size);  // 使用sum_array函数保证代码通用性，在CNN中size等于一个卷积核输出的特征图大小，在DNN中size等于1
        }
    }
}

/*
输入：卷积层 l，网络参数 net
功能：输入一个batch的数据，完成(一层)卷积层的前向计算
输出：一个batch的输出 l.output
返回：无
说明：net.input 已经在外部network.c的forward_network函数中被赋值
*/
void forward_convolutional_layer(convolutional_layer l, network net)
{
    int i, j;

    fill_cpu(l.outputs*l.batch, 0, l.output, 1);  // 将l.output以0全部填充，防止上次输入batch个数据前向计算的结果对本次造成影响

    if(l.xnor){
        binarize_weights(l.weights, l.n, l.c/l.groups*l.size*l.size, l.binary_weights);
        swap_binary(&l);
        binarize_cpu(net.input, l.c*l.h*l.w*l.batch, l.binary_input);
        net.input = l.binary_input;
    }

    int m = l.n/l.groups;  // l.n是filters数量，除以l.groups得到每组含有多少个filters
    int k = l.size*l.size*l.c/l.groups;  // 将卷积核分组后，每个卷积核的参数总数
    int n = l.out_w*l.out_h; // 每次卷积输出的高和宽的乘积
    for(i = 0; i < l.batch; ++i){  // 一个batch中的每一个样本
        for(j = 0; j < l.groups; ++j){ // 每组
            // 指针加上数值代表地址偏移，l.nweights/l.groups代表每组权重总数
            // l.weights = calloc(c/groups*n*size*size, sizeof(float));
            // 因为 *a 的取值和 i 无关，所以推测对于每组batch内，卷积核参数相同。例如batch=3，则在每个batch中，与输入数据卷积的卷积核是同一个卷积核
            // 此时的 *a 指向的是当前样本的当前组的权重
            float *a = l.weights + j*l.nweights/l.groups;
            // 大小为 l.out_h*l.out_w*l.size*l.size*l.c,存放重排后的输入数据
            float *b = net.workspace;
            // n*m表示每组每次卷积输出的大小，(i*l.groups + j) 表示进行了多少次卷积，所以 *b 表示当前样本当前组的输出位置
            float *c = l.output + (i*l.groups + j)*n*m;
            // net.input 表示当前层的输入(batchsize 个样本)，l.c/l.groups*l.h*l.w表示每次每组输入的大小,所以 *im为当前样本当前组的输入数据位置
            float *im =  net.input + (i*l.groups + j)*l.c/l.groups*l.h*l.w;
            
            // 对图片进行重新排列，指针b 指向重新排列后的数据
            if (l.size == 1) {
                // TODO: im的大小为l.c/l.groups*l.h*l.w，而下面gemm中b的大小为l.size*l.size*l.c/l.groups X l.out_w*l.out_h，当步长不为1或者有补零的时候，l.h*l.w不等于l.out_w*l.out_h
                b = im;
            } else {
                im2col_cpu(im, l.c/l.groups, l.h, l.w, l.size, l.stride, l.pad, b); // l.h, l.w 为输入大小
            }
            /*
            M 每组filters的数量
            N 每次卷积输出的高和宽的乘积
            K 将卷积核分组后，每个卷积核的参数总数
            ALPHA 广义矩阵乘积操作(gemm)参数
            *A 每次batch每组权重数组的首地址
            lda 将卷积核分组后，每个卷积核的参数总数
            *B 重排后的图像矩阵
            ldb 每次卷积输出的高和宽的乘积
            BETA 广义矩阵乘积操作(gemm)参数
            *C 每组每次卷积输出的数组首地址
            ldc 每次卷积输出的高和宽的乘积，用于定位卷积输出数组元素的位置

            广义矩阵乘积操作(gemm) C = ALPHA*A*B + BETA*C
            这里ALPHA和BETA均取值为1，完成 l.c/l.groups 组卷积操作，怀疑此处的 BETA并没有什么作用，因此每次 c 均是全零的
            */
            gemm(0,0,m,n,k,1,a,k,b,n,1,c,n);
        }
    }

    if(l.batch_normalize){
        forward_batchnorm_layer(l, net);   // 对该层的输出进行batch_normalize，该函数中已经调用了add_bias函数。在激励函数前进行
    } else {
        add_bias(l.output, l.biases, l.batch, l.n, l.out_h*l.out_w); // 对每一个输出点加上偏置
    }

    activate_array(l.output, l.outputs*l.batch, l.activation);  // 对l.output 中每一个点均经过激活函数
    if(l.binary || l.xnor) swap_binary(&l);
}

/*
输入：卷积层 l，网络参数 net
功能：一个batch的数据，完成卷积层的反向传播
输出：
返回：无
说明：对于非输入层 net.delta 已经在外部network.c的backward_network函数中被赋值
*/
void backward_convolutional_layer(convolutional_layer l, network net)
{
    int i, j;
    int m = l.n/l.groups; // l.n是filters数量，除以l.groups得到每组filters的数量
    int n = l.size*l.size*l.c/l.groups;  // 将卷积核分组后，每个卷积核的参数总数
    int k = l.out_w*l.out_h;  // 每次卷积输出的高和宽的乘积

    // 对数组l.output中所有元素关于net(o = f(net), f为激活函数)的梯度，并与之前得到的(上一层的backward函数) l.delta 做点乘，得到完整的 l.delta
    gradient_array(l.output, l.outputs*l.batch, l.activation, l.delta);

    if(l.batch_normalize){
        backward_batchnorm_layer(l, net);  // 对batchnormal 中的gamma和beta参数求偏导数，并更新 l.delta
    } else {
        backward_bias(l.bias_updates, l.delta, l.batch, l.n, k);  // 求卷积层输出 关于 偏置 b 的导数 
    }

    // net.delta 已经在外部network.c的backward_network函数中被赋值
    for(i = 0; i < l.batch; ++i){  // 每个样本
        for(j = 0; j < l.groups; ++j){ // 每组
            // m*k为每个样本每组输出大小，所以 *a 为当前样本当前组的 delta值
            float *a = l.delta + (i*l.groups + j)*m*k;   
            float *b = net.workspace;   // net.workspace 可以看做为缓冲空间
            // l.nweights/l.groups为每组的权重大小，又因为一个batch之内的相同组的权重相同，与i无关，所以 *c表示存储当前组权重更新的位置
            float *c = l.weight_updates + j*l.nweights/l.groups;
            
            // l.c/l.groups*l.h*l.w为每组输入的大小，所以 *im 为当前样本当前组的输入
            float *im  = net.input + (i*l.groups + j)*l.c/l.groups*l.h*l.w;
            // l.c/l.groups*l.h*l.w为每组输入的大小，所以 *imd 为当前样本当前组的 delta 值
            float *imd = net.delta + (i*l.groups + j)*l.c/l.groups*l.h*l.w;

            if(l.size == 1){
                // TODO:
                b = im;
            } else {
                // l.c/l.groups 为每组卷积核的个数
                im2col_cpu(im, l.c/l.groups, l.h, l.w,  
                        l.size, l.stride, l.pad, b);
            }
            // 因为这里的 *im 展开后有 out_c*out*w 列等于k的值，有l.size*l.size*l.c/l.groups行等于n的值，也就是说 *im 有 n*k列，所以需要转置
            gemm(0,1,m,n,k,1,a,k,b,k,1,c,n);   // 这里beta=1也就是每次结果都加上C是为了将一个batch之内的所有损失函数关于 W 的导数进行求和
            // 当为输入层的时候，不需要求前一层的net.delta，在backward_network函数中没有被赋值，也就不会进入此循环
            if (net.delta) {
                // *c表示当前组权重位置
                a = l.weights + j*l.nweights/l.groups;
                // *b 为当前样本当前组的 delta值
                b = l.delta + (i*l.groups + j)*m*k;
                c = net.workspace;  // 下面beta的值等于0，会刷新掉当前net.workspace的值
                if (l.size == 1) {
                    c = imd;
                }
                // a高度l.n/l.groups宽度为l.size*l.size*l.c/l.groups，也就是 m*n；b的高度为l.n/l.groups宽度为l.out_w*l.out_h也就是 m*k
                // 所以a要加上转置，beta的值等于0
                // TODO:
                gemm(1,0,n,k,m,1,a,n,b,k,0,c,k);

                if (l.size != 1) {
                    col2im_cpu(net.workspace, l.c/l.groups, l.h, l.w, l.size, l.stride, l.pad, imd);
                }
            }
        }
    }
}

void update_convolutional_layer(convolutional_layer l, update_args a)
{
    float learning_rate = a.learning_rate*l.learning_rate_scale;
    float momentum = a.momentum;
    float decay = a.decay;
    int batch = a.batch;

    axpy_cpu(l.n, learning_rate/batch, l.bias_updates, 1, l.biases, 1);
    scal_cpu(l.n, momentum, l.bias_updates, 1);

    if(l.scales){
        axpy_cpu(l.n, learning_rate/batch, l.scale_updates, 1, l.scales, 1);
        scal_cpu(l.n, momentum, l.scale_updates, 1);
    }

    axpy_cpu(l.nweights, -decay*batch, l.weights, 1, l.weight_updates, 1);
    axpy_cpu(l.nweights, learning_rate/batch, l.weight_updates, 1, l.weights, 1);
    scal_cpu(l.nweights, momentum, l.weight_updates, 1);
}


image get_convolutional_weight(convolutional_layer l, int i)
{
    int h = l.size;
    int w = l.size;
    int c = l.c/l.groups;
    return float_to_image(w,h,c,l.weights+i*h*w*c);
}

void rgbgr_weights(convolutional_layer l)
{
    int i;
    for(i = 0; i < l.n; ++i){
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            rgbgr_image(im);
        }
    }
}

void rescale_weights(convolutional_layer l, float scale, float trans)
{
    int i;
    for(i = 0; i < l.n; ++i){
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            scale_image(im, scale);
            float sum = sum_array(im.data, im.w*im.h*im.c);
            l.biases[i] += sum*trans;
        }
    }
}

image *get_weights(convolutional_layer l)
{
    image *weights = calloc(l.n, sizeof(image));
    int i;
    for(i = 0; i < l.n; ++i){
        weights[i] = copy_image(get_convolutional_weight(l, i));
        normalize_image(weights[i]);
        /*
           char buff[256];
           sprintf(buff, "filter%d", i);
           save_image(weights[i], buff);
         */
    }
    //error("hey");
    return weights;
}

image *visualize_convolutional_layer(convolutional_layer l, char *window, image *prev_weights)
{
    image *single_weights = get_weights(l);
    show_images(single_weights, l.n, window);

    image delta = get_convolutional_image(l);
    image dc = collapse_image_layers(delta, 1);
    char buff[256];
    sprintf(buff, "%s: Output", window);
    //show_image(dc, buff);
    //save_image(dc, buff);
    free_image(dc);
    return single_weights;
}

