#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <gstreamer_backend.h>
#include <gstreamer_event_recorder.h>
#include <pthread.h>

ANativeWindow *native_window;

// Java callbacks
static jmethodID on_player_init_id;
static jmethodID on_state_changed_id;
static jmethodID on_volume_changed_id;
static jmethodID on_uri_changed_id;
static jmethodID on_eos_id;
static jmethodID on_element_error_id;
static jmethodID on_recording_finished_id;
static jmethodID on_event_saved_id;

// Global context
pthread_t gst_app_thread;
static gboolean gst_app_thread_started = FALSE;
static pthread_key_t current_jni_env;
jobject app;

static JavaVM *jvm;

/* Register this thread with the VM */
static JNIEnv *attach_current_thread(void) {
    JNIEnv *env;
    JavaVMAttachArgs args;

    args.version = JNI_VERSION_1_6;
    args.name = NULL;
    args.group = NULL;

    if ((*jvm)->AttachCurrentThread(jvm, &env, &args) < 0) {
        GST_ERROR("Failed to attach current thread");
        return NULL;
    }

    return env;
}

/* Unregister this thread from the VM */
static void detach_current_thread(void *env) {
    GST_DEBUG("Detaching thread %p", g_thread_self());
    (*jvm)->DetachCurrentThread(jvm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *get_jni_env(void) {
    JNIEnv *env;

    if ((env = pthread_getspecific(current_jni_env)) == NULL) {
        env = attach_current_thread();
        pthread_setspecific(current_jni_env, env);
    }

    return env;
}

// Bindings methods
static jstring native_rct_gst_get_gstreamer_info(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    char *version_utf8 = rct_gst_get_info();
    jstring *version_jstring = (*env)->NewStringUTF(env, version_utf8);
    g_free (version_utf8);
    return version_jstring;
}

static void native_rct_gst_set_drawable_surface(JNIEnv* env, jobject thiz, jobject surface)
{
    (void)env;
    (void)thiz;

    // ANativeWindow_fromSurface acquires a reference on every call (this runs on
    // every surfaceChanged), so release the one we were holding. Do it AFTER the
    // sink has been pointed at the new window. Releasing unconditionally is
    // correct even when the same Surface yields the same pointer: that call
    // acquired a second reference which is exactly what we drop here.
    ANativeWindow *previous_window = native_window;
    native_window = ANativeWindow_fromSurface(env, surface);
    rct_gst_set_drawable_surface((guintptr)native_window);
    if (previous_window)
        ANativeWindow_release(previous_window);
}

static void native_rct_gst_set_pipeline_state(JNIEnv* env, jobject thiz, jint state)
{
    (void)env;
    (void)thiz;

    rct_gst_set_pipeline_state((GstState) state);
}

static void native_rct_gst_set_uri(JNIEnv* env, jobject thiz, jstring uri_j)
{
    (void)env;
    (void)thiz;

    // rct_gst_set_uri copies the string, so it is safe to release it here.
    const char *uri = (*env)->GetStringUTFChars(env, uri_j, 0);
    rct_gst_set_uri((gchar *)uri);
    (*env)->ReleaseStringUTFChars(env, uri_j, uri);
}

static void native_rct_gst_set_audio_level_refresh_rate(JNIEnv* env, jobject thiz, jint audio_level_refresh_rate)
{
    (void)env;
    (void)thiz;
    rct_gst_set_audio_level_refresh_rate(audio_level_refresh_rate);
}

static void native_rct_gst_set_debugging(JNIEnv* env, jobject thiz, jboolean is_debugging)
{
    (void)env;
    (void)thiz;
    rct_gst_set_debugging(is_debugging);
}

static void native_rct_gst_start_recording(JNIEnv* env, jobject thiz, jstring path_j, jint video_event_pre_length, jint video_event_post_length)
{
    (void)thiz;
    const char *path = (*env)->GetStringUTFChars(env, path_j, 0);
    rct_gst_start_recording((const gchar *)path);
    rct_gst_event_recorder_set_buffering(TRUE, (gint)video_event_pre_length, (gint)video_event_post_length);
    (*env)->ReleaseStringUTFChars(env, path_j, path);
}

static void native_rct_gst_stop_recording(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    rct_gst_stop_recording();
}

static void native_rct_gst_save_event(JNIEnv* env, jobject thiz, jstring path_j)
{
    (void)thiz;
    const char *path = (*env)->GetStringUTFChars(env, path_j, 0);
    rct_gst_event_recorder_save((const gchar *)path);
    (*env)->ReleaseStringUTFChars(env, path_j, path);
}


void native_on_init()
{
    JNIEnv *env = get_jni_env();
    (*env)->CallVoidMethod(env, app, on_player_init_id);
}

void native_on_state_changed(GstState old_state, GstState new_state)
{
    JNIEnv *env = get_jni_env();
    (*env)->CallVoidMethod(env, app, on_state_changed_id, (jint)old_state, (jint)new_state);
}

void native_on_volume_changed(RctGstAudioLevel* audioLevel)
{
    JNIEnv *env = get_jni_env();
    (*env)->CallVoidMethod(env, app, on_volume_changed_id, audioLevel->rms, audioLevel->peak, audioLevel->decay);
}

void native_on_uri_changed(gchar *_new_uri)
{
    JNIEnv *env = get_jni_env();
    jstring new_uri = (*env)->NewStringUTF(env, _new_uri);
    (*env)->CallVoidMethod(env, app, on_uri_changed_id, new_uri);
}

void native_on_eos()
{
    JNIEnv *env = get_jni_env();
    (*env)->CallVoidMethod(env, app, on_eos_id);
}

void native_on_element_error(gchar *_source, gchar *_message, gchar *_debug_info)
{
    JNIEnv *env = get_jni_env();

    jstring source = (*env)->NewStringUTF(env, _source);
    jstring message = (*env)->NewStringUTF(env, _message);
    jstring debug_info = (*env)->NewStringUTF(env, _debug_info);
    (*env)->CallVoidMethod(env, app, on_element_error_id, source, message, debug_info);
}

void native_on_recording_finished()
{
    JNIEnv *env = get_jni_env();
    (*env)->CallVoidMethod(env, app, on_recording_finished_id);
}

void native_on_event_saved(gchar *file_path)
{
    JNIEnv *env = get_jni_env();
    jstring path = (*env)->NewStringUTF(env, file_path ? file_path : "");
    (*env)->CallVoidMethod(env, app, on_event_saved_id, path);
    (*env)->DeleteLocalRef(env, path);
}

static void native_rct_gst_init_and_run(JNIEnv* env, jobject thiz, jobject j_configuration)
{
    RctGstConfiguration* configuration = rct_gst_get_configuration();
    jclass configuration_class = (*env)->GetObjectClass(env, j_configuration);

    // Creating native code internal data gst_app_thread
    app = (*env)->NewGlobalRef(env, thiz);

    // Defining initial drawable surface (ids)
    jfieldID ids_field_id = (*env)->GetFieldID(env, configuration_class, "initialDrawableSurface", "Landroid/view/Surface;");
    jobject surface = (*env)->GetObjectField(env, j_configuration, ids_field_id);
    if (native_window)
        ANativeWindow_release(native_window);   // re-init without a terminate
    native_window = ANativeWindow_fromSurface(env, surface);
    configuration->initialDrawableSurface = (guintptr)native_window;

    // Getting all callbacks
    jclass klass = (*env)->FindClass(env, "com/kalyzee/rctgstplayer/RCTGstPlayerController");
    on_player_init_id = (*env)->GetMethodID(env, klass, "onInit", "()V");
    on_state_changed_id = (*env)->GetMethodID(env, klass, "onStateChanged", "(II)V");
    on_volume_changed_id = (*env)->GetMethodID(env, klass, "onVolumeChanged", "(DDD)V");
    on_uri_changed_id = (*env)->GetMethodID(env, klass, "onUriChanged", "(Ljava/lang/String;)V");
    on_eos_id = (*env)->GetMethodID(env, klass, "onEOS", "()V");
    on_element_error_id = (*env)->GetMethodID(env, klass, "onElementError", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    on_recording_finished_id = (*env)->GetMethodID(env, klass, "onRecordingFinished", "()V");
    on_event_saved_id = (*env)->GetMethodID(env, klass, "onEventSaved", "(Ljava/lang/String;)V");

    configuration->onInit = native_on_init;
    configuration->onStateChanged = native_on_state_changed;
    configuration->onVolumeChanged = native_on_volume_changed;
    configuration->onUriChanged = native_on_uri_changed;
    configuration->onEOS = native_on_eos;
    configuration->onElementError = native_on_element_error;
    configuration->onRecordingFinished = native_on_recording_finished;
    configuration->onEventSaved = native_on_event_saved;

    rct_gst_init(configuration);

    if (pthread_create(&gst_app_thread, NULL, (void *)&rct_gst_run_loop, NULL) == 0)
        gst_app_thread_started = TRUE;
    else
        GST_ERROR("Failed to start the GStreamer main loop thread");
}

// Tear everything down. Called from RCTGstPlayerController.terminate(), i.e. from
// onDropViewInstance on the UI thread.
static void native_rct_gst_terminate(JNIEnv* env, jobject thiz)
{
    (void)thiz;

    // Stops the pipeline (closing the RTSP session and releasing the hardware
    // H.264 encoder session), cancels the bus watch and asks the GMainLoop to quit.
    rct_gst_terminate();

    // Join before releasing the window and the global ref: the loop thread makes
    // JNI upcalls through `app`, and the video sink draws into native_window.
    // Safe from the UI thread - those upcalls only dispatch RN events
    // asynchronously, so they never block on this thread.
    if (gst_app_thread_started) {
        pthread_join(gst_app_thread, NULL);
        gst_app_thread_started = FALSE;
    }

    if (native_window) {
        ANativeWindow_release(native_window);
        native_window = NULL;
    }

    if (app) {
        (*env)->DeleteGlobalRef(env, app);
        app = NULL;
    }
}

static JNINativeMethod native_methods[] = {
        { "nativeRCTGstGetGStreamerInfo", "()Ljava/lang/String;", (void *) native_rct_gst_get_gstreamer_info },

        { "nativeRCTGstInitAndRun", "(Lcom/kalyzee/rctgstplayer/utils/RCTGstConfiguration;)V", (void *) native_rct_gst_init_and_run },
        { "nativeRCTGstTerminate", "()V", (void *) native_rct_gst_terminate },

        { "nativeRCTGstSetPipelineState", "(I)V", (void *) native_rct_gst_set_pipeline_state },

        { "nativeRCTGstSetDrawableSurface", "(Landroid/view/Surface;)V", (void *) native_rct_gst_set_drawable_surface },
        { "nativeRCTGstSetUri", "(Ljava/lang/String;)V", (void *) native_rct_gst_set_uri },
        { "nativeRCTGstSetAudioLevelRefreshRate", "(I)V", (void *) native_rct_gst_set_audio_level_refresh_rate },
        { "nativeRCTGstSetDebugging", "(Z)V", (void *) native_rct_gst_set_debugging },

        { "nativeRCTGstStartRecording", "(Ljava/lang/String;II)V", (void *) native_rct_gst_start_recording },
        { "nativeRCTGstStopRecording", "()V", (void *) native_rct_gst_stop_recording },
        { "nativeRCTGstSaveEvent", "(Ljava/lang/String;)V", (void *) native_rct_gst_save_event }
};

// Called by JNI
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;

    // Storing global context
    jvm = vm;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "RCTGstPlayer", "Could not retrieve JNIEnv");
        return 0;
    }
    jclass klass = (*env)->FindClass(env, "com/kalyzee/rctgstplayer/RCTGstPlayerController");
    (*env)->RegisterNatives(env, klass, native_methods, G_N_ELEMENTS(native_methods));

    pthread_key_create(&current_jni_env, detach_current_thread);

    return JNI_VERSION_1_6;
}