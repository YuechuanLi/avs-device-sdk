/*
 * MockExceptionEncounteredSenderInterface.h
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#ifndef ALEXA_CLIENT_SDK_AVS_COMMON_TEST_AVS_COMMON_MOCK_EXCEPTION_ENCOUNTERED_SENDER_H_
#define ALEXA_CLIENT_SDK_AVS_COMMON_TEST_AVS_COMMON_MOCK_EXCEPTION_ENCOUNTERED_SENDER_H_

#include "AVSCommon/ExceptionEncounteredSenderInterface.h"
#include <gmock/gmock.h>

namespace alexaClientSDK {
namespace avsCommon {

/**
 * Mock class that implements the ExceptionEncounteredSenderInterface.
 */
class MockExceptionEncounteredSender : public ExceptionEncounteredSenderInterface {
public:
    MOCK_METHOD3(sendExceptionEncountered, void(const std::string& unparsedDirective, ExceptionErrorType error,
        const std::string& errorDescription));
};

}  // namespace avsCommon
}  // namespace alexaClientSDK

#endif // ALEXA_CLIENT_SDK_AVS_COMMON_TEST_AVS_COMMON_MOCK_EXCEPTION_ENCOUNTERED_SENDER_H_