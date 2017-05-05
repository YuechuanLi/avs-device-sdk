/*
* DirectiveProcessor.cpp
*
* Copyright 2017 Amazon.com, Inc. or its affiliates.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <algorithm>
#include <iostream>
#include <sstream>

#include <AVSCommon/ExceptionEncountered.h>
#include <AVSUtils/Logger/LogEntry.h>
#include <AVSUtils/Logging/Logger.h>
#include <AVSUtils/Memory/Memory.h>

#include "ADSL/DirectiveProcessor.h"

/// String to identify log entries originating from this file.
static const std::string TAG("DirectiveProcessor");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsUtils::logger::LogEntry(TAG, event)

namespace alexaClientSDK {
namespace adsl {

using namespace avsCommon;

std::mutex DirectiveProcessor::m_handleMapMutex;
DirectiveProcessor::ProcessorHandle DirectiveProcessor::m_nextProcessorHandle = 0;
std::unordered_map<DirectiveProcessor::ProcessorHandle, DirectiveProcessor*> DirectiveProcessor::m_handleMap;

DirectiveProcessor::DirectiveProcessor(DirectiveRouter* directiveRouter) :
        m_directiveRouter{directiveRouter},
        m_isShuttingDown{false},
        m_isHandlingDirective{false} {
    std::lock_guard<std::mutex> lock(m_handleMapMutex);
    m_handle = ++m_nextProcessorHandle;
    m_handleMap[m_handle] = this;
    m_processingThread = std::thread(&DirectiveProcessor::processingLoop, this);
}

DirectiveProcessor::~DirectiveProcessor() {
    shutdown();
}

void DirectiveProcessor::setDialogRequestId(const std::string& dialogRequestId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (dialogRequestId == m_dialogRequestId) {
        ACSDK_WARN(LX("setDialogRequestIdIgnored").d("reason", "unchanged").d("dialogRequestId", dialogRequestId));
        return;
    }
    ACSDK_INFO(LX("setDialogRequestId").d("dialogRequestId", dialogRequestId));
    queueAllDirectivesForCancellationLocked();
    m_dialogRequestId = dialogRequestId;
}

bool DirectiveProcessor::onDirective(std::shared_ptr<AVSDirective> directive) {
    if (!directive) {
        ACSDK_ERROR(LX("onDirectiveFailed").d("action", "ignored").d("reason", "nullptrDirective"));
        return false;
    }
    std::lock_guard<std::mutex> onDirectiveLock(m_onDirectiveMutex);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_isShuttingDown) {
        ACSDK_WARN(LX("onDirectiveFailed")
                .d("messageId", directive->getMessageId()).d("action", "ignored").d("reason", "shuttingDown"));
        return false;
    }
    if (directive->getDialogRequestId() != m_dialogRequestId) {
        ACSDK_INFO(LX("onDirective")
                        .d("messageId", directive->getMessageId())
                        .d("action", "dropped")
                        .d("reason", "dialogRequestIdDoesNotMatch")
                        .d("directivesDialogRequestId", directive->getDialogRequestId())
                        .d("dialogRequestId", m_dialogRequestId));
        return true;
    }
    m_directiveBeingPreHandled = directive;
    lock.unlock();
    auto handled = m_directiveRouter->preHandleDirective(
            directive, alexaClientSDK::avsUtils::memory::make_unique<DirectiveHandlerResult>(m_handle, directive));
    lock.lock();
    if (m_directiveBeingPreHandled) {
        m_directiveBeingPreHandled.reset();
        if (handled) {
            m_handlingQueue.push_back(directive);
            m_wakeProcessingLoop.notify_one();
        }
    }

    return handled;
}

void DirectiveProcessor::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_handleMapMutex);
        m_handleMap.erase(m_handle);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queueAllDirectivesForCancellationLocked();
        m_isShuttingDown = true;
        m_wakeProcessingLoop.notify_one();
    }
    if (m_processingThread.joinable()) {
        m_processingThread.join();
    }
}

DirectiveProcessor::DirectiveHandlerResult::DirectiveHandlerResult(
        DirectiveProcessor::ProcessorHandle processorHandle, std::shared_ptr<AVSDirective> directive) :
    m_processorHandle{processorHandle}, m_messageId{directive->getMessageId()} {
}

void DirectiveProcessor::DirectiveHandlerResult::setCompleted() {
    std::lock_guard<std::mutex> lock(m_handleMapMutex);
    auto it = m_handleMap.find(m_processorHandle);
    if (it == m_handleMap.end()) {
        ACSDK_DEBUG(LX("setCompletedIgnored").d("reason", "directiveSequencerAlreadyShutDown"));
        return;
    }
    it->second->onHandlingCompleted(m_messageId);
}

void DirectiveProcessor::DirectiveHandlerResult::setFailed(const std::string& description) {
    std::lock_guard<std::mutex> lock(m_handleMapMutex);
    auto it = m_handleMap.find(m_processorHandle);
    if (it == m_handleMap.end()) {
        ACSDK_DEBUG(LX("setFailedIgnored").d("reason", "directiveSequencerAlreadyShutDown"));
        return;
    }
    it->second->onHandlingFailed(m_messageId, description);
}

void DirectiveProcessor::onHandlingCompleted(const std::string& messageId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ACSDK_DEBUG(LX("onHandlingCompeted").d("messageId", messageId).d("directiveBeingPreHandled",
            m_directiveBeingPreHandled ? m_directiveBeingPreHandled->getMessageId() : "(nullptr)"));

    if (m_directiveBeingPreHandled && m_directiveBeingPreHandled->getMessageId() == messageId) {
        m_directiveBeingPreHandled.reset();
    } else if (!removeFromHandlingQueueLocked(messageId)) {
        removeFromCancelingQueueLocked(messageId);
    }
}

void DirectiveProcessor::onHandlingFailed(const std::string& messageId, const std::string& description) {
    std::unique_lock<std::mutex> lock(m_mutex);
    ACSDK_DEBUG(LX("onHandlingFailed")
            .d("messageId", messageId)
            .d("directiveBeingPreHandled",
                    m_directiveBeingPreHandled ? m_directiveBeingPreHandled->getMessageId() : "(nullptr)")
            .d("description", description));

    if (m_directiveBeingPreHandled && m_directiveBeingPreHandled->getMessageId() == messageId) {
        m_directiveBeingPreHandled.reset();
        queueAllDirectivesForCancellationLocked();
    } else if (removeFromHandlingQueueLocked(messageId)) {
        queueAllDirectivesForCancellationLocked();
    } else {
        removeFromCancelingQueueLocked(messageId);
    }
}

bool DirectiveProcessor::removeFromHandlingQueueLocked(const std::string& messageId) {
    auto it = findDirectiveInQueueLocked(messageId, m_handlingQueue);
    if (m_handlingQueue.end() == it) {
        return false;
    }
    if (m_isHandlingDirective && m_handlingQueue.begin() == it) {
        m_isHandlingDirective = false;
    }
    removeDirectiveFromQueueLocked(it, m_handlingQueue);
    return true;
}

bool DirectiveProcessor::removeFromCancelingQueueLocked(const std::string& messageId) {
    auto it = findDirectiveInQueueLocked(messageId, m_cancelingQueue);
    if (m_cancelingQueue.end() == it) {
        return false;
    }
    removeDirectiveFromQueueLocked(it, m_cancelingQueue);
    return true;
}

std::deque<std::shared_ptr<avsCommon::AVSDirective>>::iterator DirectiveProcessor::findDirectiveInQueueLocked(
        const std::string& messageId,
        std::deque<std::shared_ptr<avsCommon::AVSDirective>>& queue) {
    auto matches = [messageId](std::shared_ptr<AVSDirective> element) {
        return element->getMessageId() == messageId;
    };
    return std::find_if(queue.begin(), queue.end(), matches);
}

void DirectiveProcessor::removeDirectiveFromQueueLocked(
        std::deque<std::shared_ptr<avsCommon::AVSDirective>>::iterator it,
        std::deque<std::shared_ptr<avsCommon::AVSDirective>>& queue) {
    queue.erase(it);
    if (!queue.empty()) {
        m_wakeProcessingLoop.notify_one();
    }
}

void DirectiveProcessor::processingLoop() {
    auto wake = [this]() {
        return !m_cancelingQueue.empty() || (!m_handlingQueue.empty() && !m_isHandlingDirective) || m_isShuttingDown;
    };

    while (true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wakeProcessingLoop.wait(lock, wake);
        if (!processCancelingQueueLocked(lock) && !handleDirectiveLocked(lock) && m_isShuttingDown) {
            break;
        }
    }
}

bool DirectiveProcessor::processCancelingQueueLocked(std::unique_lock<std::mutex> &lock) {
    if (m_cancelingQueue.empty()) {
        return false;
    }
    std::deque<std::shared_ptr<avsCommon::AVSDirective>> temp(std::move(m_cancelingQueue));
    lock.unlock();
    for (auto directive : temp) {
        m_directiveRouter->cancelDirective(directive);
    }
    lock.lock();
    return true;
}

bool DirectiveProcessor::handleDirectiveLocked(std::unique_lock<std::mutex> &lock) {
    if (m_handlingQueue.empty()) {
        return false;
    }
    if (m_isHandlingDirective) {
        return true;
    }
    auto directive = m_handlingQueue.front();
    m_isHandlingDirective = true;
    lock.unlock();
    auto policy = BlockingPolicy::NONE;
    auto handled = m_directiveRouter->handleDirective(directive, &policy);
    lock.lock();
    if (!handled || BlockingPolicy::BLOCKING != policy) {
        m_isHandlingDirective = false;
        if (!m_handlingQueue.empty() && m_handlingQueue.front() == directive) {
            m_handlingQueue.pop_front();
        }  else if (!handled) {
            ACSDK_ERROR(LX("handlingDirectiveLockedFailed")
                    .d("expected", directive->getMessageId())
                    .d("front", m_handlingQueue.empty() ? "(empty)" : m_handlingQueue.front()->getMessageId())
                    .d("reason", "handlingQueueFrontChangedWithoutBeingHandled"));
        }
    }
    if (!handled) {
        queueAllDirectivesForCancellationLocked();
    }
    return true;
}

void DirectiveProcessor::queueAllDirectivesForCancellationLocked() {
    m_dialogRequestId.clear();
    if (m_directiveBeingPreHandled) {
        m_handlingQueue.push_back(m_directiveBeingPreHandled);
        m_directiveBeingPreHandled.reset();
    }
    if (!m_handlingQueue.empty()) {
        m_cancelingQueue.insert(m_cancelingQueue.end(), m_handlingQueue.begin(), m_handlingQueue.end());
        m_handlingQueue.clear();
        m_wakeProcessingLoop.notify_one();
    }
    m_isHandlingDirective = false;
}

} // namespace directiveSequencer
} // namespace alexaClientSDK
