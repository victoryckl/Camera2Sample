// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <android/log.h>

#include <jni.h>

#include <string>
#include <vector>

#include <platform.h>
#include <benchmark.h>

#include "blazeface.h"

#include "ndkcamera.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

static int draw_unsupported(cv::Mat& rgb)
{
    const char text[] = "unsupported";

    int baseLine = 0;
    cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 1, &baseLine);

    int y = (rgb.rows - label_size.height) / 2;
    int x = (rgb.cols - label_size.width) / 2;

    cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                    cv::Scalar(255, 255, 255), -1);

    cv::putText(rgb, text, cv::Point(x, y + label_size.height),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 0));

    return 0;
}

static int draw_fps(cv::Mat& rgb)
{
    // resolve moving average
    float avg_fps = 0.f;
    {
        static double t0 = 0.f;
        static float fps_history[10] = {0.f};

        double t1 = ncnn::get_current_time();
        if (t0 == 0.f)
        {
            t0 = t1;
            return 0;
        }

        float fps = 1000.f / (t1 - t0);
        t0 = t1;

        for (int i = 9; i >= 1; i--)
        {
            fps_history[i] = fps_history[i - 1];
        }
        fps_history[0] = fps;

        if (fps_history[9] == 0.f)
        {
            return 0;
        }

        for (int i = 0; i < 10; i++)
        {
            avg_fps += fps_history[i];
        }
        avg_fps /= 10.f;
    }

    char text[32];
    sprintf(text, "FPS=%.2f", avg_fps);

    int baseLine = 0;
    cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

    int y = 0;
    int x = rgb.cols - label_size.width;

    cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                    cv::Scalar(255, 255, 255), -1);

    cv::putText(rgb, text, cv::Point(x, y + label_size.height),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));

    return 0;
}

static BlazeFace* g_blazeface = 0;
static ncnn::Mutex lock;

class MyNdkCamera : public NdkCameraWindow
{
public:
    virtual void on_image_render(cv::Mat& rgb) const;
};

void MyNdkCamera::on_image_render(cv::Mat& rgb) const
{
    // scrfd
    {
        ncnn::MutexLockGuard g(lock);

        if (g_blazeface)
        {
            std::vector<FaceObject> faceobjects;
            g_blazeface->detect(rgb, faceobjects);

            g_blazeface->draw(rgb, faceobjects);
        }
        else
        {
            draw_unsupported(rgb);
        }
    }

    draw_fps(rgb);
}

static MyNdkCamera* g_camera = 0;

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "JNI_OnLoad");

    g_camera = new MyNdkCamera;

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "JNI_OnUnload");

    {
        ncnn::MutexLockGuard g(lock);

        delete g_blazeface;
        g_blazeface = 0;
    }

    delete g_camera;
    g_camera = 0;
}

// public native boolean loadModel(AssetManager mgr, int modelid, int cpugpu);
JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_loadModel(JNIEnv* env, jobject thiz, jobject assetManager, jint modelid, jint cpugpu)
{
    if (modelid < 0 || modelid > 6 || cpugpu < 0 || cpugpu > 1)
    {
        return JNI_FALSE;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "loadModel %p", mgr);

    const int target_sizes[] =
    {
        128,
        320,
        416,
        640
    };

    int target_size = target_sizes[(int)modelid];
    bool use_gpu = (int)cpugpu == 1;

    // reload
    {
        ncnn::MutexLockGuard g(lock);

        if (use_gpu && ncnn::get_gpu_count() == 0)
        {
            // no gpu
            delete g_blazeface;
            g_blazeface = 0;
        }
        else
        {
            if (!g_blazeface)
                g_blazeface = new BlazeFace;
            g_blazeface->load(mgr, target_size, use_gpu);
        }
    }

    return JNI_TRUE;
}

// public native boolean openCamera(int facing);
JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_openCamera(JNIEnv* env, jobject thiz, jint facing)
{
    if (facing < 0 || facing > 1)
        return JNI_FALSE;

    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "openCamera %d", facing);

    g_camera->open((int)facing);

    return JNI_TRUE;
}

// public native boolean closeCamera();
JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_closeCamera(JNIEnv* env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "closeCamera");

    g_camera->close();

    return JNI_TRUE;
}

// public native boolean setOutputWindow(Surface surface);
JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_setOutputWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);

    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "setOutputWindow %p", win);

    g_camera->set_window(win);

    return JNI_TRUE;
}

// public native boolean testCamera(int facing);
JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_testCamera(JNIEnv* env, jobject thiz)
{
    ACameraManager* camera_manager = ACameraManager_create();
    ACameraIdList* camera_id_list = 0;
    ACameraManager_getCameraIdList(camera_manager, &camera_id_list);

    __android_log_print(ANDROID_LOG_WARN, "ncnn", "testCamera: numCameras = %d", camera_id_list->numCameras);

    if (camera_id_list) {
        ACameraManager_deleteCameraIdList(camera_id_list);
        camera_id_list = 0;
    }


    if (camera_manager)
    {
        ACameraManager_delete(camera_manager);
        camera_manager = 0;
    }

    return JNI_TRUE;
}


void on_image_render(cv::Mat& rgb) {
    // scrfd
    {
        ncnn::MutexLockGuard g(lock);

        if (g_blazeface) {
            std::vector<FaceObject> faceobjects;
            g_blazeface->detect(rgb, faceobjects);
            if (faceobjects.size() > 0) {
                __android_log_print(ANDROID_LOG_WARN, "ncnn",
                                    "on_image_render: faceobjects.size = %d", faceobjects.size()+0);
                g_blazeface->draw(rgb, faceobjects);
            }
        } else {
            draw_unsupported(rgb);
        }
    }

    draw_fps(rgb);
}

int haveFace(cv::Mat& rgb) {
    ncnn::MutexLockGuard g(lock);
    if (g_blazeface) {
        std::vector<FaceObject> faceobjects;
        g_blazeface->detect(rgb, faceobjects);
        return faceobjects.size();
    } else {
        //draw_unsupported(rgb);
    }
    return 0;
}

cv::Mat bz_yuv420sp2rgb_rotate(const unsigned char* nv21, int nv21_width, int nv21_height) {
    int camera_orientation = 270;
    int camera_facing = 1;

    // rotate nv21
    int w = 0;
    int h = 0;
    int rotate_type = 0;
    {
        if (camera_orientation == 0)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 2 : 1;
        }
        if (camera_orientation == 90)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 5 : 6;
        }
        if (camera_orientation == 180)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 4 : 3;
        }
        if (camera_orientation == 270)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 7 : 8;
        }
    }

    cv::Mat nv21_rotated(h + h / 2, w, CV_8UC1);
    ncnn::kanna_rotate_yuv420sp(nv21, nv21_width, nv21_height, nv21_rotated.data, w, h, rotate_type);

    // nv21_rotated to rgb
    cv::Mat rgb(h, w, CV_8UC3);
    ncnn::yuv420sp2rgb(nv21_rotated.data, w, h, rgb.data);

    return rgb;
}

cv::Mat bz_yuv420sp2rgb(const unsigned char* nv21, int nv21_width, int nv21_height) {
    // rotate nv21
    int w = nv21_width;
    int h = nv21_height;

    // nv21_rotated to rgb
    cv::Mat rgb(h, w, CV_8UC3);
    ncnn::yuv420sp2rgb(nv21, w, h, rgb.data);

    return rgb;
}

void on_image(const unsigned char* nv21, int nv21_width, int nv21_height)
{
    int camera_orientation = 270;
    int camera_facing = 1;

    // rotate nv21
    int w = 0;
    int h = 0;
    int rotate_type = 0;
    {
        if (camera_orientation == 0)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 2 : 1;
        }
        if (camera_orientation == 90)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 5 : 6;
        }
        if (camera_orientation == 180)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 4 : 3;
        }
        if (camera_orientation == 270)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 7 : 8;
        }
    }

    cv::Mat nv21_rotated(h + h / 2, w, CV_8UC1);
    ncnn::kanna_rotate_yuv420sp(nv21, nv21_width, nv21_height, nv21_rotated.data, w, h, rotate_type);

    // nv21_rotated to rgb
    cv::Mat rgb(h, w, CV_8UC3);
    ncnn::yuv420sp2rgb(nv21_rotated.data, w, h, rgb.data);

    on_image_render(rgb);
}

JNIEXPORT jint JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_find(
        JNIEnv* env, jobject thiz, jbyteArray buffer, jint width, jint height)
{
    //__android_log_print(ANDROID_LOG_WARN, "ncnn", "find: w = %d, h = %d", width, height);

    uint8_t * data = (uint8_t *)(env->GetByteArrayElements(buffer, NULL));

    cv::Mat rgb = bz_yuv420sp2rgb(data, width, height);
    int count = haveFace(rgb);

    env->ReleaseByteArrayElements(buffer, (int8_t*) data, 0);
    return count;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_blazefacencnn_BlazeFaceNcnn_renderImage2(
        JNIEnv* env, jobject thiz, jobject image) {
    return false;
}



} // extern "C" {
