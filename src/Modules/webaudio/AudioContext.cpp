/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "LabSoundConfig.h"

#include "AudioContext.h"

#include "AnalyserNode.h"
#include "AsyncAudioDecoder.h"
#include "AudioBuffer.h"
#include "AudioBufferCallback.h"
#include "AudioBufferSourceNode.h"
#include "AudioContextLock.h"
#include "AudioListener.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "BiquadFilterNode.h"
#include "ChannelMergerNode.h"
#include "ChannelSplitterNode.h"
#include "ConvolverNode.h"
#include "DefaultAudioDestinationNode.h"
#include "DelayNode.h"
#include "DynamicsCompressorNode.h"
#include "FFTFrame.h"
#include "GainNode.h"
#include "HRTFDatabaseLoader.h"
#include "HRTFPanner.h"
#include "OfflineAudioDestinationNode.h"
#include "OscillatorNode.h"
#include "PannerNode.h"
#include "WaveShaperNode.h"
#include "WaveTable.h"

#include "MediaStream.h"
#include "MediaStreamAudioDestinationNode.h"
#include "MediaStreamAudioSourceNode.h"

#if DEBUG_AUDIONODE_REFERENCES
#include <stdio.h>
#endif

#include <wtf/Atomics.h>
#include <wtf/MainThread.h>

using namespace std;

namespace WebCore {
    
namespace {
    
bool isSampleRateRangeGood(float sampleRate)
{
    // FIXME: It would be nice if the minimum sample-rate could be less than 44.1KHz,
    // but that will require some fixes in HRTFPanner::fftSizeForSampleRate(), and some testing there.
    return sampleRate >= 44100 && sampleRate <= 96000;
}

}

// Don't allow more than this number of simultaneous AudioContexts talking to hardware.
const int MaxHardwareContexts = 4;
int AudioContext::s_hardwareContextCount = 0;
    
std::unique_ptr<AudioContext> AudioContext::create(ExceptionCode&)
{
    if (s_hardwareContextCount >= MaxHardwareContexts)
        return 0;

    return std::unique_ptr<AudioContext>(new AudioContext());
}

std::unique_ptr<AudioContext> AudioContext::createOfflineContext(unsigned numberOfChannels, size_t numberOfFrames, float sampleRate, ExceptionCode& ec)
{
    // FIXME: offline contexts have limitations on supported sample-rates.
    // Currently all AudioContexts must have the same sample-rate.
    auto loader = HRTFDatabaseLoader::loader();
    if (numberOfChannels > 10 || !isSampleRateRangeGood(sampleRate) || (loader && loader->databaseSampleRate() != sampleRate)) {
        ec = SYNTAX_ERR;
        return 0;
    }

    return std::unique_ptr<AudioContext>(new AudioContext(numberOfChannels, numberOfFrames, sampleRate));
}

// Constructor for rendering to the audio hardware.
AudioContext::AudioContext()
    : m_isStopScheduled(false)
    , m_isInitialized(false)
    , m_isAudioThreadFinished(false)
    , m_destinationNode(0)
    , m_isDeletionScheduled(false)
    , m_automaticPullNodesNeedUpdating(false)
    , m_connectionCount(0)
    , m_isOfflineContext(false)
    , m_activeSourceCount(0)
{
    constructCommon();
}

// Constructor for offline (non-realtime) rendering.
AudioContext::AudioContext(unsigned numberOfChannels, size_t numberOfFrames, float sampleRate)
    : m_isStopScheduled(false)
    , m_isInitialized(false)
    , m_isAudioThreadFinished(false)
    , m_destinationNode(0)
    , m_automaticPullNodesNeedUpdating(false)
    , m_connectionCount(0)
    , m_isOfflineContext(true)
    , m_activeSourceCount(0)
{
    constructCommon();

    // FIXME: the passed in sampleRate MUST match the hardware sample-rate since HRTFDatabaseLoader is a singleton.
    m_hrtfDatabaseLoader = HRTFDatabaseLoader::createAndLoadAsynchronouslyIfNecessary(sampleRate);

    // Create a new destination for offline rendering.
    m_renderTarget = AudioBuffer::create(numberOfChannels, numberOfFrames, sampleRate);
    // a destination node must be created before this context can be used
}

void AudioContext::initHRTFDatabase() {
    
    // This sets in motion an asynchronous loading mechanism on another thread.
    // We can check m_hrtfDatabaseLoader->isLoaded() to find out whether or not it has been fully loaded.
    // It's not that useful to have a callback function for this since the audio thread automatically starts rendering on the graph
    // when this has finished (see AudioDestinationNode).
    m_hrtfDatabaseLoader = HRTFDatabaseLoader::createAndLoadAsynchronouslyIfNecessary(sampleRate());
}

void AudioContext::constructCommon()
{
    FFTFrame::initialize();
    
    m_listener = std::make_shared<AudioListener>();
}

AudioContext::~AudioContext()
{
#if DEBUG_AUDIONODE_REFERENCES
    fprintf(stderr, "%p: AudioContext::~AudioContext()\n", this);
#endif
    
    ASSERT(!m_isInitialized);
    ASSERT(m_isStopScheduled);
    ASSERT(!m_nodesToDelete.size());
    ASSERT(!m_referencedNodes.size());
    ASSERT(!m_finishedNodes.size());
    ASSERT(!m_automaticPullNodes.size());
    ASSERT(!m_renderingAutomaticPullNodes.size());
}

void AudioContext::lazyInitialize()
{
    if (!m_isInitialized) {
        // Don't allow the context to initialize a second time after it's already been explicitly uninitialized.
        ASSERT(!m_isAudioThreadFinished);
        if (!m_isAudioThreadFinished) {
            if (m_destinationNode.get()) {
                m_destinationNode->initialize();

                if (!isOfflineContext()) {
                    // This starts the audio thread. The destination node's provideInput() method will now be called repeatedly to render audio.
                    // Each time provideInput() is called, a portion of the audio stream is rendered. Let's call this time period a "render quantum".
                    // NOTE: for now default AudioContext does not need an explicit startRendering() call from JavaScript.
                    // We may want to consider requiring it for symmetry with OfflineAudioContext.
                    m_destinationNode->startRendering();
                    atomicIncrement(&s_hardwareContextCount);
                }

            }
            m_isInitialized = true;
        }
    }
}

void AudioContext::clear()
{
    // Audio thread is dead. Nobody will schedule node deletion action. Let's do it ourselves.
    do {
        deleteMarkedNodes();
        m_nodesToDelete.insert(m_nodesToDelete.end(), m_nodesMarkedForDeletion.begin(), m_nodesMarkedForDeletion.end());
        m_nodesMarkedForDeletion.clear();
    } while (m_nodesToDelete.size());
}

void AudioContext::uninitialize(ContextGraphLock& g)
{
    if (!m_isInitialized)
        return;

    // This stops the audio thread and all audio rendering.
    m_destinationNode->uninitialize();

    // Don't allow the context to initialize a second time after it's already been explicitly uninitialized.
    m_isAudioThreadFinished = true;

    if (!isOfflineContext()) {
        ASSERT(s_hardwareContextCount);
        atomicDecrement(&s_hardwareContextCount);
    }

    m_referencedNodes.clear();
    m_isInitialized = false;

}

bool AudioContext::isInitialized() const
{
    return m_isInitialized;
}

    size_t AudioContext::currentSampleFrame() const { return m_destinationNode->currentSampleFrame(); }
    double AudioContext::currentTime() const { return m_destinationNode->currentTime(); }
    float AudioContext::sampleRate() const { return m_destinationNode ? m_destinationNode->sampleRate() : AudioDestination::hardwareSampleRate(); }
    
    void AudioContext::incrementConnectionCount()
    {
        atomicIncrement(&m_connectionCount);    // running tally
    }

    
bool AudioContext::isRunnable() const
{
    if (!isInitialized())
        return false;
    
    // Check with the HRTF spatialization system to see if it's finished loading.
    return m_hrtfDatabaseLoader->isLoaded();
}

void AudioContext::stop(ContextGraphLock& g)
{
    if (m_isStopScheduled)
        return;
    
    m_isStopScheduled = true;
    
    uninitialize(g);
    clear();
}

void AudioContext::decodeAudioData(std::shared_ptr<std::vector<uint8_t>> audioData,
                                   PassRefPtr<AudioBufferCallback> successCallback, PassRefPtr<AudioBufferCallback> errorCallback, ExceptionCode& ec)
{
    if (!audioData) {
        ec = SYNTAX_ERR;
        return;
    }
    m_audioDecoder->decodeAsync(audioData, sampleRate(), successCallback, errorCallback);
}

std::shared_ptr<MediaStreamAudioSourceNode> AudioContext::createMediaStreamSource(ContextGraphLock& g, ContextRenderLock& r, ExceptionCode& ec)
{
    std::shared_ptr<MediaStream> mediaStream = std::make_shared<MediaStream>();

    AudioSourceProvider* provider = 0;

    if (mediaStream->isLocal() && mediaStream->audioTracks()->length())
        provider = destination()->localAudioInputProvider();
    else {
        // FIXME: get a provider for non-local MediaStreams (like from a remote peer).
        provider = 0;
    }

    std::shared_ptr<MediaStreamAudioSourceNode> node(new MediaStreamAudioSourceNode(mediaStream, provider, sampleRate()));

    // FIXME: Only stereo streams are supported right now. We should be able to accept multi-channel streams.
    node->setFormat(g, r, 2, sampleRate());

    m_referencedNodes.push_back(node); // context keeps reference until node is disconnected
    return node;
}

void AudioContext::notifyNodeFinishedProcessing(ContextRenderLock& r, AudioNode* node)
{
    ASSERT(r.context());

    for (auto i : m_referencedNodes) {
        if (i.get() == node) {
            m_finishedNodes.push_back(i);
            return;
        }
    }
    ASSERT(0 == "node to finish not referenced");
}

void AudioContext::derefFinishedSourceNodes(ContextGraphLock& g)
{
    ASSERT(g.context());
    for (unsigned i = 0; i < m_finishedNodes.size(); i++)
        derefNode(g, m_finishedNodes[i]);

    m_finishedNodes.clear();
}

void AudioContext::refNode(ContextGraphLock& g, std::shared_ptr<AudioNode> node)
{
    m_referencedNodes.push_back(node);
}

void AudioContext::derefNode(ContextGraphLock& g, std::shared_ptr<AudioNode> node)
{
    ASSERT(g.context());

    for (std::vector<std::shared_ptr<AudioNode>>::iterator i = m_referencedNodes.begin(); i != m_referencedNodes.end(); ++i) {
        if (node == *i) {
            m_referencedNodes.erase(i);
            break;
        }
    }
}
    
void AudioContext::holdSourceNodeUntilFinished(std::shared_ptr<AudioScheduledSourceNode> sn) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    automaticSources.push_back(sn);
}
    
void AudioContext::handleAutomaticSources() {
    lock_guard<mutex> lock(automaticSourcesMutex);
    for (auto i = automaticSources.begin(); i != automaticSources.end(); ++i) {
        if ((*i)->hasFinished()) {
            i = automaticSources.erase(i);
            if (i == automaticSources.end())
                break;
        }
    }
}

void AudioContext::handlePreRenderTasks(ContextRenderLock& r)
{
    ASSERT(r.context());
 
    // At the beginning of every render quantum, try to update the internal rendering graph state (from main thread changes).
    AudioSummingJunction::handleDirtyAudioSummingJunctions(r);
    updateAutomaticPullNodes();
}

void AudioContext::connect(std::shared_ptr<AudioNode> from, std::shared_ptr<AudioNode> to) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    pendingNodeConnections.emplace_back(from, to, true);
}
void AudioContext::disconnect(std::shared_ptr<AudioNode> from, std::shared_ptr<AudioNode> to) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    pendingNodeConnections.emplace_back(from, to, false);
}
void AudioContext::disconnect(std::shared_ptr<AudioNode> from) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    pendingNodeConnections.emplace_back(from, std::shared_ptr<AudioNode>(), false);
}
    
void AudioContext::connect(std::shared_ptr<AudioNodeInput> fromInput, std::shared_ptr<AudioNodeOutput> toOutput) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    pendingConnections.emplace_back(PendingConnection(fromInput, toOutput, true));
}
void AudioContext::disconnect(std::shared_ptr<AudioNodeOutput> toOutput) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    pendingConnections.emplace_back(PendingConnection(std::shared_ptr<AudioNodeInput>(), toOutput, false));
}

void AudioContext::update(ContextGraphLock& g) {
    {
        lock_guard<mutex> lock(automaticSourcesMutex);
        for (auto i : pendingConnections) {
            if (i.connect) {
                AudioNodeInput::connect(g, i.fromInput, i.toOutput);
            }
            else {
                AudioNodeOutput::disconnectAll(g, i.toOutput);
            }
        }
        pendingConnections.clear();
        
        for (auto i : pendingNodeConnections) {
            if (i.connect) {
                AudioNodeInput::connect(g, i.to->input(0), i.from->output(0));
                refNode(g, i.from);
                refNode(g, i.to);
                atomicIncrement(&i.from->m_connectionRefCount);
                atomicIncrement(&i.to->m_connectionRefCount);
                i.from->enableOutputsIfNecessary(g);
                i.to->enableOutputsIfNecessary(g);
            }
            else {
                if (!i.from) {
                    ExceptionCode ec = NO_ERR;
                    atomicDecrement(&i.to->m_connectionRefCount);
                    i.to->disconnect(g.context(), 0, ec);
                    i.to->disableOutputsIfNecessary(g);
                }
                else {
                    atomicDecrement(&i.from->m_connectionRefCount);
                    atomicDecrement(&i.to->m_connectionRefCount);
                    AudioNodeInput::disconnect(g, i.from->input(0), i.to->output(0));
                    derefNode(g, i.from);
                    derefNode(g, i.to);
                    i.from->disableOutputsIfNecessary(g);
                    i.to->disableOutputsIfNecessary(g);
                }
            }
        }
        pendingNodeConnections.clear();
    }
    
    // Dynamically clean up nodes which are no longer needed.
    derefFinishedSourceNodes(g);
}
    
void AudioContext::handlePostRenderTasks(ContextRenderLock& r)
{
    ASSERT(r.context());
 
    // Don't delete in the real-time thread. Let the main thread do it because the clean up may take time
    scheduleNodeDeletion(r);

    AudioSummingJunction::handleDirtyAudioSummingJunctions(r);
    updateAutomaticPullNodes();
    
    handleAutomaticSources();
}

void AudioContext::markForDeletion(ContextRenderLock& r, AudioNode* node)
{
    ASSERT(r.context());
    for (auto i : m_referencedNodes) {
        if (i.get() == node) {
            m_nodesMarkedForDeletion.push_back(i);
            return;
        }
    }
    
    ASSERT(0 == "Attempting to delete unreferenced node");
}

void AudioContext::scheduleNodeDeletion(ContextRenderLock& r)
{
    // &&& all this deletion stuff should be handled by a concurrent queue - simply have only a m_nodesToDelete concurrent queue and ditch the marked vector
    
    // then this routine sould go away completely
    
    // node->deref is the only caller, it should simply add itself to the scheduled deletion queue
    
    
    // marked for deletion should go away too
    
    bool isGood = m_isInitialized && r.context();
    ASSERT(isGood);
    if (!isGood)
        return;

    // Make sure to call deleteMarkedNodes() on main thread.    
    if (m_nodesMarkedForDeletion.size() && !m_isDeletionScheduled) {
        m_nodesToDelete.insert(m_nodesToDelete.end(), m_nodesMarkedForDeletion.begin(), m_nodesMarkedForDeletion.end());
        m_nodesMarkedForDeletion.clear();

        m_isDeletionScheduled = true;

        // Don't let ourself get deleted before the callback.
        // See matching deref() in deleteMarkedNodesDispatch().
        callOnMainThread(deleteMarkedNodesDispatch, this);
    }
}

void AudioContext::deleteMarkedNodesDispatch(void* userData)
{
    AudioContext* context = reinterpret_cast<AudioContext*>(userData);
    ASSERT(context);
    if (!context)
        return;

    context->deleteMarkedNodes();
}

void AudioContext::deleteMarkedNodes()
{
    m_nodesToDelete.clear();
    m_isDeletionScheduled = false;
}


void AudioContext::addAutomaticPullNode(std::shared_ptr<AudioNode> node) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    if (m_automaticPullNodes.find(node) == m_automaticPullNodes.end()) {
        m_automaticPullNodes.insert(node);
        m_automaticPullNodesNeedUpdating = true;
    }
}

void AudioContext::removeAutomaticPullNode(std::shared_ptr<AudioNode> node) {
    lock_guard<mutex> lock(automaticSourcesMutex);
    auto it = m_automaticPullNodes.find(node);
    if (it != m_automaticPullNodes.end()) {
        m_automaticPullNodes.erase(it);
        m_automaticPullNodesNeedUpdating = true;
    }
}

void AudioContext::updateAutomaticPullNodes()
{
    if (m_automaticPullNodesNeedUpdating) {
        lock_guard<mutex> lock(automaticSourcesMutex);
        
        // Copy from m_automaticPullNodes to m_renderingAutomaticPullNodes.
        m_renderingAutomaticPullNodes.resize(m_automaticPullNodes.size());

        unsigned j = 0;
        for (auto i = m_automaticPullNodes.begin(); i != m_automaticPullNodes.end(); ++i, ++j) {
            m_renderingAutomaticPullNodes[j] = *i;
        }

        m_automaticPullNodesNeedUpdating = false;
    }
}

void AudioContext::processAutomaticPullNodes(ContextRenderLock& r, size_t framesToProcess)
{
    for (unsigned i = 0; i < m_renderingAutomaticPullNodes.size(); ++i)
        m_renderingAutomaticPullNodes[i]->processIfNecessary(r, framesToProcess);
}

void AudioContext::startRendering()
{
    destination()->startRendering();
}

void AudioContext::fireCompletionEvent()
{
    // this is called when an offline audio destination has finished rendering. And nothing happens.
}

void AudioContext::incrementActiveSourceCount()
{
    atomicIncrement(&m_activeSourceCount);
}

void AudioContext::decrementActiveSourceCount()
{
    atomicDecrement(&m_activeSourceCount);
}

} // namespace WebCore
