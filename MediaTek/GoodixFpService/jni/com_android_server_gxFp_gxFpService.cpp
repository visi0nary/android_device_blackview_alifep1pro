/*
 **
 ** Copyright 2008, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <stdio.h>
#include <fp.h>
#include <android_runtime/AndroidRuntime.h>
#include <utils/Mutex.h>
#include "jni.h"
#include <JNIHelp.h>
#include <utils/Log.h>
#include <err_code.h>
#include "utils/misc.h"

#define TAG       "android_hardware_fpdevice"
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,TAG,__VA_ARGS__)

namespace android {

#ifndef NELEM
# define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))
#endif

struct fields_t
{
    jfieldID context;
    jmethodID post_event;
};
static fields_t fields;
static Mutex 	sLock;

// provides persistent context for calls from native code to Java
class JNIFpDeviceContext: public FpListener
{
public:
    JNIFpDeviceContext(JNIEnv* env, jobject weak_this, jclass clazz,
                       const sp<Fp>& fp);
    ~JNIFpDeviceContext()
    {
        release();
    }
    sp<Fp> getFp()
    {
        Mutex::Autolock _l(mLock);
        return mFp;
    }
    virtual void notify(int32_t msgType, int32_t ext1, int32_t ext2);
    virtual void notifyData(int32_t msgType, int32_t length, char *data);
    void release();

private:
    jobject mFpDeviceJObjectWeak;     	// weak reference to java object
    jclass mFpDeviceJClass;          	// strong reference to java class
    sp<Fp> mFp;                			// strong reference to native object
    Mutex mLock;
};

JNIFpDeviceContext::JNIFpDeviceContext(JNIEnv* env, jobject weak_this,
                                       jclass clazz, const sp<Fp>& fp)
{
    mFpDeviceJObjectWeak = env->NewGlobalRef(weak_this);
    mFpDeviceJClass = (jclass) env->NewGlobalRef(clazz);
    mFp = fp;
}
void JNIFpDeviceContext::release()
{

}

void JNIFpDeviceContext::notify(int32_t msgType, int32_t ext1, int32_t ext2)
{
    if (mFpDeviceJObjectWeak == NULL)
    {
        return;
    }
    JNIEnv *env = AndroidRuntime::getJNIEnv();
    /*
     * If the notification or msgType is CAMERA_MSG_RAW_IMAGE_NOTIFY, change it
     * to CAMERA_MSG_RAW_IMAGE since CAMERA_MSG_RAW_IMAGE_NOTIFY is not exposed
     * to the Java app.
     */
    env->CallStaticVoidMethod(mFpDeviceJClass, fields.post_event,
                              mFpDeviceJObjectWeak, msgType, ext1, ext2, NULL);
}
void JNIFpDeviceContext::notifyData(int32_t msgType, int32_t length, char *data)
{
    if (mFpDeviceJObjectWeak == NULL)
    {
        return;
    }
    jbyteArray obj = NULL;
    JNIEnv *env = AndroidRuntime::getJNIEnv();
    const jbyte* jdata = reinterpret_cast<const jbyte*>(data);

    obj = env->NewByteArray(length);
    env->SetByteArrayRegion(obj, 0, length, jdata);
    env->CallStaticVoidMethod(mFpDeviceJClass, fields.post_event,
                              mFpDeviceJObjectWeak, msgType, 0, 0, obj);
    if (obj)
    {
        env->DeleteLocalRef(obj);
    }
}

sp<Fp> get_native_fp(JNIEnv *env, jobject thiz, JNIFpDeviceContext** pContext)
{
    sp<Fp> FpDevice;
    Mutex::Autolock _l(sLock);
    JNIFpDeviceContext* jnicontext =
        reinterpret_cast<JNIFpDeviceContext*>(env->GetIntField(thiz,
                fields.context));
    if (jnicontext != NULL)
    {
        FpDevice = jnicontext->getFp();
    }
    ALOGV("get_native_fp: context=%p, fp=%p", jnicontext, FpDevice.get());
    if (FpDevice == 0)
    {
        jniThrowRuntimeException(env, "Method called after release()");
    }

    if (pContext != NULL)
        *pContext = jnicontext;
    return FpDevice;
}

struct field
{
    const char *class_name;
    const char *field_name;
    const char *field_type;
    jfieldID *jfield;
};

static field fields_to_find[] =
{
    {"com/goodix/device/FpDevice", "mNativeContext", "I", &fields.context}
};

static int find_fields(JNIEnv *env, field *fields, int count)
{
    for (int i = 0; i < count; i++)
    {
        field *f = &fields[i];
        jclass clazz = env->FindClass(f->class_name);
        if (clazz == NULL)
        {
            return -1;
        }

        jfieldID field = env->GetFieldID(clazz, f->field_name, f->field_type);
        if (field == NULL)
        {
            return -1;
        }

        *(f->jfield) = field;
    }
    return 0;
}

static void native_1setup(
    JNIEnv *env, jobject thiz, jobject weak_ref)
{
    sp<Fp> fp = Fp::connect(0);

    if (fp == NULL)
    {
        jniThrowRuntimeException(env, "Fail to connect to FpService");
        return;
    }

    if (fp->getStatus() != NO_ERROR)
    {
        jniThrowRuntimeException(env, "FpService initialization failed");
        return;
    }

    jclass clazz = env->GetObjectClass(thiz);
    if (clazz == NULL)
    {
        jniThrowRuntimeException(env, "Can't find com/goodix/device/FpDevice");
        return;
    }

    fields.post_event = env->GetStaticMethodID(clazz, "postEventFromNative",
                        "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    sp<JNIFpDeviceContext> context = new JNIFpDeviceContext(env, weak_ref,
            clazz, fp);
    context->incStrong(thiz);

    fp->setListener(context);

    find_fields(env, fields_to_find, NELEM(fields_to_find));
    env->SetIntField(thiz, fields.context, (int) context.get());
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    query
 * Signature: ()I
 */
static jint query(JNIEnv *env,
        jobject thiz)
{
    ALOGV("query");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_query();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    register
 * Signature: ()I
 */
static jint Register(JNIEnv *env,
        jobject thiz)
{
    ALOGV("register");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_Register();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    cancelRegister
 * Signature: ()I
 */
static jint cancelRegister(
    JNIEnv *env, jobject thiz)
{
    ALOGV("Java_com_goodix_device_FpDevice_cancelRegister!");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_RegisterCancel();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    resetRegister
 * Signature: ()I
 */
static jint resetRegister(
    JNIEnv *env, jobject thiz)
{
    ALOGV("Java_com_goodix_device_FpDevice_resetRegister");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_ResetRegister();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    match
 * Signature: ()I
 */
static jint match(JNIEnv *env,
        jobject thiz)
{
    ALOGV("Java_com_goodix_device_FpDevice_match");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_Match();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    delete
 * Signature: ()I
 */
static jint Delete(JNIEnv *env,
        jobject thiz, jint index)
{
    ALOGV("Java_com_goodix_device_FpDevice_delete");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_UnRegister((int) index);
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    getInfo
 * Signature: ()Ljava/lang/String;
 */
static jstring getInfo(JNIEnv *env,
        jobject thiz)
{
    ALOGV("Java_com_goodix_device_FpDevice_getInfo");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    const char* info = fp->gx_GetInfo();
    return env->NewStringUTF(info);
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    checkPassword
 * Signature: (Ljava/lang/String;)Z
 */
static jint checkPassword(
    JNIEnv *env, jobject thiz, jstring password)
{
    LOGD("Java_com_goodix_device_FpDevice_checkPassword");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    const char *pwd = env->GetStringUTFChars(password, 0);

    LOGD("checkPasswrod pwd = %s,length = %d", pwd, strlen(pwd));
    status_t result = SUCCESS;
    result = fp->gx_CheckPWD(pwd);
    LOGD("checkPasswrod : pwd = %s result = %d", pwd, result);
    env->ReleaseStringUTFChars(password, pwd);
    return result;
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    changePassword
 * Signature: (Ljava/lang/String;Ljava/lang/String;)Z
 */
static jint changePassword(
    JNIEnv *env, jobject thiz, jstring oldPsw, jstring newPsw)
{
    LOGD("Java_com_goodix_device_FpDevice_changePassword");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    const char *pwd = env->GetStringUTFChars(oldPsw, 0);
    const char *newPwd = env->GetStringUTFChars(newPsw, 0);
    status_t result = SUCCESS;
    result = fp->gx_SetPWD(pwd, newPwd);
    LOGD("changePasswrod : pwd = %s", pwd);
    env->ReleaseStringUTFChars(oldPsw, pwd);
    env->ReleaseStringUTFChars(newPsw, newPwd);
    return result;
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    getPermission
 * Signature: (Ljava/lang/String;)I
 */
static jint getPermission(
    JNIEnv *env, jobject thiz, jstring string)
{
    LOGD("Java_com_goodix_device_FpDevice_getPermission");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    const char *pwd = env->GetStringUTFChars(string, 0);
    status_t result = SUCCESS;
    result = fp->gx_getPermission(pwd);
    env->ReleaseStringUTFChars(string, pwd);
    return result;
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    saveRegister
 * Signature: (I)I
 */
static jint saveRegister(
    JNIEnv *env, jobject thiz, jint index)
{
    LOGD("Java_com_goodix_device_FpDevice_saveRegister");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    status_t result = SUCCESS;
    result = fp->gx_RegisterSave((int) index);
    return result;
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    EngTest
 * Signature: (I)I
 */
static jint setMode(JNIEnv *env,
        jobject thiz, jint cmd)
{
    LOGD("Java_com_goodix_device_FpDevice_setMode");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    status_t result = SUCCESS;
    result = fp->gx_EngTest((int) cmd);
    return result;
}
/*
 * Class:     com_goodix_device_FpDevice
 * Method:    SendCmd
 * Signature: (I;Ljava/lang/String;Ljava/lang/String;)I
 */
static jint SendCmd(JNIEnv *env,
        jobject thiz, jint cmd, jstring jarg1, jstring jarg2)
{
    LOGD("Java_com_goodix_device_FpDevice_SendCmd");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    status_t result = SUCCESS;
    const char *arg1 = env->GetStringUTFChars(jarg1, 0);
    const char *arg2 = env->GetStringUTFChars(jarg2, 0);    
    result = fp->gx_SendCmd((int)cmd,arg1,arg2);
    env->ReleaseStringUTFChars(jarg1, arg1);
    env->ReleaseStringUTFChars(jarg2,arg2);
    return result;
}
/*
 * Class:     com_goodix_device_FpDevice
 * Method:    getPermission
 * Signature: (Ljava/lang/String;)I
 */
static jint recognize(JNIEnv *env,
        jobject thiz)
{
    LOGD("Java_com_goodix_device_FpDevice_recognize");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    status_t result = SUCCESS;
    result = fp->gx_Match();
    return result;
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    cancelRecognize
 * Signature: ()I
 */
static jint cancelRecognize(JNIEnv *env, jobject thiz)
{
    LOGD("Java_com_goodix_device_FpDevice_cancelRecognize");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_MatchCancel();
}

/*
 * Class:     com_goodix_device_FpDevice
 * Method:    sendScreenState
 * Signature: (I)I
 */
static jint sendScreenState(
    JNIEnv *env, jobject thiz, jint state)
{
    ALOGV("Java_com_goodix_device_FpDevice_sendScreenState");
    JNIFpDeviceContext* jnicontext;
    sp<Fp> fp = get_native_fp(env, thiz, &jnicontext);
    if (fp == 0)
    {
        jniThrowRuntimeException(env, "FpService not initialization!");
    }
    return fp->gx_sendScreenState((int) state);

}

    static const JNINativeMethod method_table[] = {  
        {"cancelRecognize_native", "()I", (void*)cancelRecognize},  
        {"cancelRegister_native", "()I", (void*)cancelRegister},  
        {"changePassword_native", "(Ljava/lang/String;Ljava/lang/String;)I", (void*)changePassword},  
        {"checkPassword_native", "(Ljava/lang/String;)I", (void*)checkPassword},  
        {"delete_native", "(I)I", (void*)Delete},  
        {"getInfo_native", "()Ljava/lang/String;", (void*)getInfo},
        {"getPermission_native", "(Ljava/lang/String;)I", (void*)getPermission},  
  //      {"match_native", "()I", (void*)match},  
        {"native_setup", "(Ljava/lang/Object;)V", (void*)native_1setup},  
        {"query_native", "()I", (void*)query},  
        {"recognize_native", "()I", (void*)recognize},  
        {"register_native", "()I", (void*)Register},
				{"resetRegister_native", "()I", (void*)resetRegister},  
				{"saveRegister_native", "(I)I", (void*)saveRegister},  
    //    {"SendCmd_native", "(Ljava/lang/String;Ljava/lang/String)I", (void*)SendCmd},  
        {"sendScreenState_native", "(I)I", (void*)sendScreenState},
        {"setMode_native", "(I)I", (void*)setMode},
     };  
extern "C" {
int register_com_goodix_service_gxFpService(JNIEnv *env) 
{  
	return jniRegisterNativeMethods(env, "com/goodix/device/FpDevice", method_table, NELEM(method_table));  
}  
    
jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;
    jint result = -1;

    if (vm->GetEnv((void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        ALOGE("GetEnv failed!");
        return result;
    }
    ALOG_ASSERT(env, "Could not retrieve the env!");
//add by hy for gxfp
		register_com_goodix_service_gxFpService(env);	
//add by hy end
    return JNI_VERSION_1_4;
}    
}
};
