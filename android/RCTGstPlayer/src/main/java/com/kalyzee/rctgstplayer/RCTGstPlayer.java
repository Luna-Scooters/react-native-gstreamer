package com.kalyzee.rctgstplayer;

import android.view.View;


import androidx.annotation.Nullable;

import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.common.MapBuilder;
import com.facebook.react.uimanager.SimpleViewManager;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.annotations.ReactProp;
import com.kalyzee.rctgstplayer.utils.manager.Command;

import java.util.Map;

/**
 * Created by asapone on 02/01/2018.
 */

public class RCTGstPlayer extends SimpleViewManager {

    private RCTGstPlayerController playerController;

    @Override
    public String getName() {
        return "RCTGstPlayer";
    }

    @Override
    protected View createViewInstance(ThemedReactContext reactContext) {
        this.playerController = new RCTGstPlayerController(reactContext);
        return this.playerController.getView();
    }

    @Override
    public void onDropViewInstance(View view) {
        // The view is going away: shut the pipeline down before React Native
        // releases the surface it draws into.
        if (this.playerController != null && this.playerController.getView() == view) {
            this.playerController.terminate();
            this.playerController = null;
        }
        super.onDropViewInstance(view);
    }

    // Shared properties
    @ReactProp(name = "uri")
    public void setUri(View controllerView, String uri) {
        this.playerController.setRctGstUri(uri);
    }

    @ReactProp(name = "audioLevelRefreshRate")
    public void setAudioLevelRefreshRate(View controllerView, int audioLevelRefreshRate) {
        this.playerController.setRctGstAudioLevelRefreshRate(audioLevelRefreshRate);
    }

    @ReactProp(name = "isDebugging")
    public void setAudioLevelRefreshRate(View controllerView, boolean isDebugging) {
        this.playerController.setRctGstDebugging(isDebugging);
    }

    @ReactProp(name = "captureFrames", defaultBoolean = false)
    public void setCaptureFrames(View controllerView, boolean captureFrames) {
        this.playerController.setCaptureFrames(captureFrames);
    }

    // Methods
    @Override
    public void receiveCommand(View view, int commandType, @Nullable ReadableArray args) {

        // setState
        if (Command.is(commandType, Command.setState))
            this.playerController.setRctGstState(args.getInt(0));

        if (Command.is(commandType, Command.startRecording))
            this.playerController.startRecording(args.getString(0), args.getInt(1), args.getInt(2));

        if (Command.is(commandType, Command.stopRecording))
            this.playerController.stopRecording();

        if (Command.is(commandType, Command.saveEvent))
            this.playerController.saveEvent(args.getString(0));

        // recreateView is ignored on purpose : Not needed on android (wrong impl of vtdec on ios)
    }

    // Commands (JS callable methods listing map)
    @Override
    public Map<String, Integer> getCommandsMap() {
        return Command.getCommandsMap();
    }

    /**
     * This method maps the sending of the "onAudioLevelChange" event to the JS "onAudioLevelChange" function.
     */

    // Events callbacks
    @Nullable
    @Override
    public Map<String, Object> getExportedCustomDirectEventTypeConstants() {
        return MapBuilder.<String, Object>builder()
                .put(
                        "onPlayerInit", MapBuilder.of("registrationName", "onPlayerInit")
                ).put(
                        "onStateChanged", MapBuilder.of("registrationName", "onStateChanged")
                ).put(
                        "onVolumeChanged", MapBuilder.of("registrationName", "onVolumeChanged")
                ).put(
                        "onUriChanged", MapBuilder.of("registrationName", "onUriChanged")
                ).put(
                        "onEOS", MapBuilder.of("registrationName", "onEOS")
                ).put(
                        "onElementError", MapBuilder.of("registrationName", "onElementError")
                ).put(
                        "onRecordingFinished", MapBuilder.of("registrationName", "onRecordingFinished")
                ).put(
                        "onEventSaved", MapBuilder.of("registrationName", "onEventSaved")
                ).build();
    }


}
