import React from 'react'
import { requireNativeComponent, UIManager, findNodeHandle, AppState } from 'react-native'

const PropTypes = require('prop-types')

export const GstState = {
    VOID_PENDING: 0,
    NULL: 1,
    READY: 2,
    PAUSED: 3,
    PLAYING: 4
}

export default class GstPlayer extends React.Component {

    currentGstState = undefined
    lastRequestedState = undefined
    appState = "active"
    isInitialized = false

    appStateSubscription = null;

    componentDidMount() {
        this.playerHandle = findNodeHandle(this.playerViewRef);
        this.appStateSubscription = AppState.addEventListener('change', this.appStateChanged);
    }

    componentWillUnmount() {
        if (this.appStateSubscription) {
            this.appStateSubscription.remove();
            this.appStateSubscription = null
        }
    }

    appStateChanged = (nextAppState) => {
        if (nextAppState !== 'active') {
            this.stopImageCapture()
        } 
        else if (this.appState.match(/inactive|background/) && this.lastRequestedState === GstState.PLAYING) {
            this.play()
        }
        this.appState = nextAppState
    }

    // Callbacks
    onPlayerInit() {
        this.isInitialized = true

        if (this.props.onPlayerInit)
            this.props.onPlayerInit()
    }

    onStateChanged(_message) {
        const { old_state, new_state } = _message.nativeEvent
        console.log(_message.nativeEvent)
        this.currentGstState = new_state

        if (this.props.onStateChanged)
            this.props.onStateChanged(old_state, new_state)
    }

    onVolumeChanged(_message) {
        const { rms, peak, decay } = _message.nativeEvent

        if (this.props.onVolumeChanged)
            this.props.onVolumeChanged(rms, peak, decay)
    }

    onUriChanged(_message) {
        const { new_uri } = _message.nativeEvent

        if (this.props.onUriChanged) {
            this.props.onUriChanged(new_uri)
        }

        if (this.props.autoPlay) {
            this.play()
        }
    }

    onEOS() {
        if (this.props.onEOS)
            this.props.onEOS()
    }

    onElementError(_message) {
        const { source, message, debug_info } = _message.nativeEvent

        if (this.props.onElementError)
            this.props.onElementError(source, message, debug_info)
    }

    onRecordingFinished() {
        if (this.props.onRecordingFinished)
            this.props.onRecordingFinished()
    }

    onEventSaved(_event) {
        if (this.props.onEventSaved)
            this.props.onEventSaved(_event.nativeEvent.path)
    }

    shouldComponentUpdate() {
        return true
    }

    // Methods
    setGstState(state) {
        this.lastRequestedState = state

        UIManager.dispatchViewManagerCommand(
            this.playerHandle,
            UIManager.RCTGstPlayer.Commands.setState,
            [state]
        )
    }

    // Player state shortcuts
    play() {
        this.setGstState(GstState.PLAYING)
    }

    pause() {
        this.setGstState(GstState.PAUSED)
    }

    stop() {
        this.setGstState(GstState.READY)
    }

    stopImageCapture() {
        UIManager.dispatchViewManagerCommand(
            this.playerHandle,
            UIManager.RCTGstPlayer.Commands.stopImageCapture,
            []
        )
    }

    startRecording(path, videoEventPreLength, videoEventPostLength) {
        UIManager.dispatchViewManagerCommand(
            this.playerHandle,
            UIManager.RCTGstPlayer.Commands.startRecording,
            [path, videoEventPreLength, videoEventPostLength]
        )
    }

    stopRecording() {
        UIManager.dispatchViewManagerCommand(
            this.playerHandle,
            UIManager.RCTGstPlayer.Commands.stopRecording,
            []
        )
    }

    saveEvent(path) {
        UIManager.dispatchViewManagerCommand(
            this.playerHandle,
            UIManager.RCTGstPlayer.Commands.saveEvent,
            [path]
        )
    }

    render() {
        return (
            <RCTGstPlayer
                autoPlay={this.props.autoPlay}
                uri={this.props.uri || undefined}
                audioLevelRefreshRate={this.props.audioLevelRefreshRate !== undefined ? this.props.audioLevelRefreshRate : 100}
                isDebugging={this.props.isDebugging !== undefined ? this.props.isDebugging : false}
                captureFrames={this.props.captureFrames !== undefined ? this.props.captureFrames : false}

                onPlayerInit={this.onPlayerInit.bind(this)}
                onStateChanged={this.onStateChanged.bind(this)}
                onVolumeChanged={this.onVolumeChanged.bind(this)}
                onUriChanged={this.onUriChanged.bind(this)}
                onEOS={this.onEOS.bind(this)}
                onElementError={this.onElementError.bind(this)}
                onRecordingFinished={this.onRecordingFinished.bind(this)}
                onEventSaved={this.onEventSaved.bind(this)}

                ref={(playerView) => this.playerViewRef = playerView}

                {...this.props}
            />
        )
    }
}

GstPlayer.propTypes = {

    // Props
    uri: PropTypes.string,
    autoPlay: PropTypes.bool,
    audioLevelRefreshRate: PropTypes.number,
    isDebugging: PropTypes.bool,
    captureFrames: PropTypes.bool,

    // Events callbacks
    onPlayerInit: PropTypes.func,
    onStateChanged: PropTypes.func,
    onVolumeChanged: PropTypes.func,
    onUriChanged: PropTypes.func,
    onEOS: PropTypes.func,
    onElementError: PropTypes.func,
    onRecordingFinished: PropTypes.func,
    onEventSaved: PropTypes.func,

    // Methods
    setGstState: PropTypes.func,
    play: PropTypes.func,
    pause: PropTypes.func,
    stop: PropTypes.func,
    stopImageCapture: PropTypes.func,
    startRecording: PropTypes.func,
    stopRecording: PropTypes.func,
    saveEvent: PropTypes.func,

    // Helper methods
    createDrawableSurface: PropTypes.func,
    destroyDrawableSurface: PropTypes.func
}

const RCTGstPlayer = requireNativeComponent('RCTGstPlayer')