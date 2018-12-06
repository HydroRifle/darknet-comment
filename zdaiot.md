`darknet`源代码是makefile管理的，之前不会在Linux调试大型项目，今天探索了一下，这里介绍一下。

## 准备工作
从[这里](https://github.com/pjreddie/darknet)下载源代码

修改makefile文件中`DEBUG=0`改为`DEBUG=1`进行调试。其中编译选项`-O0`，意思是不进行编译优化，gdb在默认情况下会使用`-O2`，会出现print变量中出现`<optimized out>`。

接着编译源代码：
```
make clean
make
```

根目录会出现`darknet`可执行文件。

在工程根目录运行如下命令下载权重：
```
wget https://pjreddie.com/media/files/yolov3-tiny.weights
```

## 开始调试
终端输入如下语句，开始调试
```
gdb ./darknet
```

在`gdb`命令中输入运行程序需要的参数类型
```
set args detect cfg/yolov3-tiny.cfg yolov3-tiny.weights data/dog.jpg
```

为了对整个工程进行调试，这里需要将`src`目录添加进来，在`gdb`命令中输入如下指令：
```
DIR ./src
```

在`gdb`命令中为`main`函数设置断点
```
b main
```

开始调试，在`gdb`命令中输入`r`，回车，发现程序停留在第一行。

接着可以在第435行，即`char *outfile = find_char_arg(argc, argv, "-out", 0);`，打上断点，在`gdb`命令中输入`c`，回车，程序跳到下一个断点，即停留在该行。输入`s`命令单步执行并跳入此处调用的子函数。输入`print 变量名`或者`p 变量名`即可查看该变量值。输入`finish`跳出子函数。输入`n`单步执行，不跳入子函数。输入`q`结束调试。


### 命令总结
```
set args detect cfg/yolov3-tiny.cfg yolov3-tiny.weights data/dog.jpg
DIR ./src
b detector.c:569
r
record
```

## 疑问
1) option_list.c中的`list *read_data_cfg(char *filename)`函数中的`strip(line);`为何可以改变line的值？即 float* 和 char *的区别
答：该问题表述有误，实际并没有改变line的值(指向)，改变的是line值指向地址的值。

2) 神经网络权重初始化问题

3) 卷积中偏置的数量以及作用
答：每一个卷积核对应一个偏置(同一个batch中同一个卷积核也是一个参数)。作用为：增加卷积的非线性能力。

4）batchnorm_layer.c中的forward_batchnorm_layer函数中 l.rolling_mean 与 l.rolling_variance 怎么求的