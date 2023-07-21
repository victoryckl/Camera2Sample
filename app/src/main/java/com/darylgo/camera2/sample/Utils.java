package com.darylgo.camera2.sample;

import android.media.Image;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;

// https://blog.csdn.net/weixin_41937380/article/details/127758173
public class Utils {
    //Planar格式（P）的处理
    private static ByteBuffer getuvBufferWithoutPaddingP(ByteBuffer uBuffer,
                              ByteBuffer vBuffer, int width, int height, int rowStride, int pixelStride) {
        int pos = 0;
        byte[] byteArray = new byte[height * width / 2];
        for (int row = 0; row < height / 2; row++) {
            for (int col = 0; col < width / 2; col++) {
                int vuPos = col * pixelStride + row * rowStride;
                byteArray[pos++] = vBuffer.get(vuPos);
                byteArray[pos++] = uBuffer.get(vuPos);
            }
        }
        ByteBuffer bufferWithoutPaddings = ByteBuffer.allocate(byteArray.length);
        // 数组放到buffer中
        bufferWithoutPaddings.put(byteArray);
        //重置 limit 和postion 值否则 buffer 读取数据不对
        bufferWithoutPaddings.flip();
        return bufferWithoutPaddings;
    }

    //Semi-Planar格式（SP）的处理和y通道的数据
    private static ByteBuffer getBufferWithoutPadding(ByteBuffer buffer,
                              int width, int rowStride, int times, boolean isVbuffer) {
        if (width == rowStride) return buffer;  //没有padding,不用处理。
        int bufferPos = buffer.position();
        int cap = buffer.capacity();
        byte[] byteArray = new byte[times * width];
        int pos = 0;
        //对于y平面，要逐行赋值的次数就是height次。对于uv交替的平面，赋值的次数是height/2次
        for (int i = 0; i < times; i++) {
            buffer.position(bufferPos);
            //part 1.1 对于u,v通道,会缺失最后一个像u值或者v值，因此需要特殊处理，否则会crash
            if (isVbuffer && i == times - 1) {
                width = width - 1;
            }
            buffer.get(byteArray, pos, width);
            bufferPos += rowStride;
            pos = pos + width;
        }

        //nv21数组转成buffer并返回
        ByteBuffer bufferWithoutPaddings = ByteBuffer.allocate(byteArray.length);
        // 数组放到buffer中
        bufferWithoutPaddings.put(byteArray);
        //重置 limit 和postion 值否则 buffer 读取数据不对
        bufferWithoutPaddings.flip();
        return bufferWithoutPaddings;
    }

    public static byte[] YUV_420_888toNV21(Image image) {
        try {
            int width = image.getWidth();
            int height = image.getHeight();
            Image.Plane yPlane = image.getPlanes()[0];
            if (yPlane == null) return null;

            ByteBuffer yBuffer = getBufferWithoutPadding(yPlane.getBuffer(),
                    image.getWidth(), yPlane.getRowStride(), image.getHeight(), false);
            ByteBuffer vBuffer;
            //part1 获得真正的消除padding的ybuffer和ubuffer。需要对P格式和SP格式做不同的处理。如果是P格式的话只能逐像素去做，性能会降低。
            Image.Plane plane1 = image.getPlanes()[1];
            Image.Plane plane2 = image.getPlanes()[2];
            if (plane2.getPixelStride() == 1) { //如果为true，说明是P格式。
                vBuffer = getuvBufferWithoutPaddingP(plane1.getBuffer(), plane2.getBuffer(),
                        width, height, plane1.getRowStride(), plane1.getPixelStride());
            } else {
                vBuffer = getBufferWithoutPadding(plane2.getBuffer(),
                        image.getWidth(), plane2.getRowStride(), image.getHeight() / 2, true);
            }

            //part2 将y数据和uv的交替数据（除去最后一个v值）赋值给nv21
            int ySize = yBuffer.remaining();
            int vSize = vBuffer.remaining();
            byte[] nv21;
            int byteSize = width * height * 3 / 2;
            nv21 = new byte[byteSize];
            yBuffer.get(nv21, 0, ySize);
            vBuffer.get(nv21, ySize, vSize);

            //part3 最后一个像素值的u值是缺失的，因此需要从u平面取一下。
            ByteBuffer uPlane = plane1.getBuffer();
            byte lastValue = uPlane.get(uPlane.capacity() - 1);
            nv21[byteSize - 1] = lastValue;
            return nv21;
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }


    //旋转摄像头采集的NV21画面，得到推流画面，顺时针为正方向
    public static void rotateNV21(byte[] src, byte[] rotatedBytes, int width, int height, int rotation) {
        if (rotation == 90)
            rotateNV21_90(src, rotatedBytes, width, height);
        else if (rotation == 180)
            rotateNV21_180(src, rotatedBytes, width, height);
        else if (rotation == 270)
            rotateNV21_270(src, rotatedBytes, width, height);
        else
            rotateNV21_0(src, rotatedBytes, width, height);
    }

    //将NV21画面原样保留
    public static void rotateNV21_0(byte[] src, byte[] dst, int width, int height) {
        System.arraycopy(src, 0, dst, 0, src.length);
    }

    //将NV21画面顺时针旋转90角度
    public static void rotateNV21_90(byte[] src, byte[] dst, int width, int height) {

        //旋转90度后的像素排列：
        //新的行数=原来的列数
        //新的列数=原来的高度-1-原来的行数

        //每相邻的四个Y分量，共享一组VU分量
        //旋转前VU分量在左上角，旋转后VU分量在右上角

        int index = 0;
        //旋转Y分量，放入dst数组
        for (int y = 0; y < width; y++)
            for (int x = 0; x < height; x++) {
                int oldY = (height - 1) - x;
                int oldX = y;
                int oldIndex = oldY * width + oldX;
                dst[index++] = src[oldIndex];
            }
        //每四个点采集一组VU分量，共享右上角像素的VU分量
        //根据Y分量，找到对应的VU分量，放入dst数组
        for (int y = 0; y < width; y += 2)
            for (int x = 0; x < height; x += 2) {
                int oldY = (height - 1) - (x + 1);
                int oldX = y;
                int vuY = height + oldY / 2; //根据Y分量计算VU分量所在行
                int vuX = oldX;
                int vuIndex = vuY * width + vuX;
                dst[index++] = src[vuIndex];
                dst[index++] = src[vuIndex + 1];
            }
    }

    //将NV21画面顺时针旋转180角度
    public static void rotateNV21_180(byte[] src, byte[] dst, int width, int height) {

        //旋转180度后的像素排列：
        //新的行数=原来的高度-1-原来的行数
        //新的列数=原来的宽度-1-原来的列数

        //每相邻的四个Y分量，共享一组VU分量
        //旋转前VU分量在左上角，旋转后VU分量在右下角

        int index = 0;
        //旋转Y分量，放入dst数组
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                int oldY = (height - 1) - y;
                int oldX = (width - 1) - x;
                int oldIndex = oldY * width + oldX;
                dst[index++] = src[oldIndex];
            }
        //每四个点采集一组VU分量，共享右下角像素的VU分量
        //根据Y分量，找到对应的VU分量，放入dst数组
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                int oldY = (height - 1) - (y + 1);
                int oldX = (width - 1) - (x + 1);
                int vuY = height + oldY / 2; //根据Y分量计算VU分量所在行
                int vuX = oldX;
                int vuIndex = vuY * width + vuX;
                dst[index++] = src[vuIndex];
                dst[index++] = src[vuIndex + 1];
            }
    }

    //将NV21画面顺时针旋转270角度
    public static void rotateNV21_270(byte[] src, byte[] dst, int width, int height) {

        //旋转270度后的像素排列：
        //新的行数=原来的宽度-1-原来的列数
        //新的列数=原来的行数

        //每相邻的四个Y分量，共享一组VU分量
        //旋转前VU分量在左上角，旋转后VU分量在左下角

        int index = 0;
        //旋转Y分量，放入dst数组
        for (int y = 0; y < width; y++)
            for (int x = 0; x < height; x++) {
                int oldY = x;
                int oldX = width - 1 - y;
                int oldIndex = oldY * width + oldX;
                dst[index++] = src[oldIndex];
            }
        //每四个点采集一组VU分量，共享左下角像素的VU分量
        //根据Y分量，找到对应的VU分量，放入dst数组
        for (int y = 0; y < width; y += 2)
            for (int x = 0; x < height; x += 2) {
                int oldY = x;
                int oldX = width - 1 - (y + 1);
                int vuY = height + oldY / 2; //根据Y分量计算VU分量所在行
                int vuX = oldX;
                int vuIndex = vuY * width + vuX;
                dst[index++] = src[vuIndex];
                dst[index++] = src[vuIndex + 1];
            }
    }

    //将NV21画面水平翻转
    public static void reverseNV21_H(byte[] src, byte[] dst, int width, int height) {

        //水平翻转的像素排列：
        //新的行数=原来的行数
        //新的列数=原来的宽度-1-原来的列数

        //每相邻的四个Y分量，共享一组VU分量
        //翻转前VU分量在左上角，旋转后VU分量在右上角

        int index = 0;
        //旋转Y分量，放入dst数组
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                int oldY = y;
                int oldX = width - 1 - x;
                int oldIndex = oldY * width + oldX;
                dst[index++] = src[oldIndex];
            }
        //每四个点采集一组VU分量，共享右上角像素的VU分量
        //根据Y分量，找到对应的VU分量，放入dst数组
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                int oldY = y;
                int oldX = width - 1 - (x + 1);
                int vuY = height + oldY / 2; //根据Y分量计算VU分量所在行
                int vuX = oldX;
                int vuIndex = vuY * width + vuX;
                dst[index++] = src[vuIndex];
                dst[index++] = src[vuIndex + 1];
            }
    }

    //将NV21画面竖直翻转
    public static void reverseNV21_V(byte[] src, byte[] dst, int width, int height) {

        //竖直翻转的像素排列：
        //新的行数=原来的高度-1-原来的行数
        //新的列数=原来的列数

        //每相邻的四个Y分量，共享一组VU分量
        //翻转前VU分量在左上角，旋转后VU分量在左下角

        int index = 0;
        //旋转Y分量，放入dst数组
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                int oldY = height - 1 - y;
                int oldX = x;
                int oldIndex = oldY * width + oldX;
                dst[index++] = src[oldIndex];
            }
        //每四个点采集一组VU分量，共享左下角像素的VU分量
        //根据Y分量，找到对应的VU分量，放入dst数组
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                int oldY = height - 1 - (y + 1);
                int oldX = x;
                int vuY = height + oldY / 2; //根据Y分量计算VU分量所在行
                int vuX = oldX;
                int vuIndex = vuY * width + vuX;
                dst[index++] = src[vuIndex];
                dst[index++] = src[vuIndex + 1];
            }
    }

    //向文件写入图像字节
    public static void writeImageBytesToFile(byte[] bytes, String path) {
        try {
            File file = new File(path);
            if (file.exists())
                file.delete();
            FileOutputStream fos = new FileOutputStream(file);
            fos.write(bytes);
            fos.flush();
            fos.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

}
