/*
 * Copyright 2014-2019 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AERON_IMAGE_H
#define AERON_IMAGE_H

#include <concurrent/AtomicBuffer.h>
#include <concurrent/logbuffer/LogBufferDescriptor.h>
#include <concurrent/logbuffer/FrameDescriptor.h>
#include <concurrent/logbuffer/Header.h>
#include <concurrent/logbuffer/TermReader.h>
#include <concurrent/logbuffer/TermBlockScanner.h>
#include <concurrent/status/UnsafeBufferPosition.h>
#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <cassert>
#include "LogBuffers.h"

namespace aeron {

using namespace aeron::concurrent;
using namespace aeron::concurrent::logbuffer;
using namespace aeron::concurrent::status;

static UnsafeBufferPosition NULL_UNSAFE_BUFFER_POSITION;

enum class ControlledPollAction : int
{
    /**
     * Abort the current polling operation and do not advance the position for this fragment.
     */
    ABORT = 1,

    /**
     * Break from the current polling operation and commit the position as of the end of the current fragment
     * being handled.
     */
    BREAK,

    /**
     * Continue processing but commit the position as of the end of the current fragment so that
     * flow control is applied to this point.
     */
    COMMIT,

    /**
     * Continue processing taking the same approach as the in fragment_handler_t.
     */
    CONTINUE
};

/**
 * Callback for handling fragments of data being read from a log.
 *
 * @param buffer containing the data.
 * @param offset at which the data begins.
 * @param length of the data in bytes.
 * @param header representing the meta data for the data.
 * @return The action to be taken with regard to the stream position after the callback.
 */
typedef std::function<ControlledPollAction(
    concurrent::AtomicBuffer &buffer,
    util::index_t offset,
    util::index_t length,
    Header &header)> controlled_poll_fragment_handler_t;

/**
 * Represents a replicated publication {@link Image} from a publisher to a {@link Subscription}.
 * Each {@link Image} identifies a source publisher by session id.
 *
 * Is an overlay on the LogBuffers and Position. So, can be effectively copied and moved.
 */
class Image
{
public:
    typedef Image this_t;
    typedef std::vector<std::shared_ptr<Image>> list_t;
    typedef std::shared_ptr<Image>* array_t;

    /**
     * Construct a new image over a log to represent a stream of messages from a {@link Publication}.
     *
     * @param sessionId                  of the stream of messages.
     * @param initialPosition            at which the subscriber is joining the stream.
     * @param subscriberPosition         for indicating the position of the subscriber in the stream.
     * @param logBuffers                 containing the stream of messages.
     * @param correlationId              of the image with the media driver.
     * @param subscriptionRegistrationId of the Subscription.
     * @param exceptionHandler           to call if an exception is encountered on polling.
     */
    Image(
        std::int32_t sessionId,
        std::int64_t correlationId,
        std::int64_t subscriptionRegistrationId,
        const std::string& sourceIdentity,
        UnsafeBufferPosition& subscriberPosition,
        std::shared_ptr<LogBuffers> logBuffers,
        const exception_handler_t& exceptionHandler) :
        m_header(
            LogBufferDescriptor::initialTermId(logBuffers->atomicBuffer(LogBufferDescriptor::LOG_META_DATA_SECTION_INDEX)),
            logBuffers->atomicBuffer(0).capacity(),
            this),
        m_subscriberPosition(subscriberPosition),
        m_logBuffers(std::move(logBuffers)),
        m_sourceIdentity(sourceIdentity),
        m_isClosed(false),
        m_exceptionHandler(exceptionHandler),
        m_correlationId(correlationId),
        m_subscriptionRegistrationId(subscriptionRegistrationId),
        m_sessionId(sessionId)
    {
        for (int i = 0; i < LogBufferDescriptor::PARTITION_COUNT; i++)
        {
            m_termBuffers[i] = m_logBuffers->atomicBuffer(i);
        }

        const util::index_t capacity = m_termBuffers[0].capacity();

        m_joinPosition = subscriberPosition.get();
        m_finalPosition = m_joinPosition;
        m_termLengthMask = capacity - 1;
        m_positionBitsToShift = BitUtil::numberOfTrailingZeroes(capacity);
        m_isEos = false;
    }

    Image(const Image& image) :
        m_termBuffers(image.m_termBuffers),
        m_header(image.m_header),
        m_subscriberPosition(image.m_subscriberPosition),
        m_logBuffers(image.m_logBuffers),
        m_sourceIdentity(image.m_sourceIdentity),
        m_isClosed(image.isClosed()),
        m_exceptionHandler(image.m_exceptionHandler),
        m_correlationId(image.m_correlationId),
        m_subscriptionRegistrationId(image.m_subscriptionRegistrationId),
        m_joinPosition(image.m_joinPosition),
        m_finalPosition(image.m_finalPosition),
        m_sessionId(image.m_sessionId),
        m_termLengthMask(image.m_termLengthMask),
        m_positionBitsToShift(image.m_positionBitsToShift),
        m_isEos(image.m_isEos)
    {
    }

    Image& operator = (const Image& image)
    {
        m_termBuffers = image.m_termBuffers;
        m_header = image.m_header;
        m_subscriberPosition = image.m_subscriberPosition;
        m_logBuffers = image.m_logBuffers;
        m_sourceIdentity = image.m_sourceIdentity;
        m_isClosed = image.isClosed();
        m_exceptionHandler = image.m_exceptionHandler;
        m_correlationId = image.m_correlationId;
        m_subscriptionRegistrationId = image.m_subscriptionRegistrationId;
        m_joinPosition = image.m_joinPosition;
        m_finalPosition = image.m_finalPosition;
        m_sessionId = image.m_sessionId;
        m_termLengthMask = image.m_termLengthMask;
        m_positionBitsToShift = image.m_positionBitsToShift;
        m_isEos = image.m_isEos;

        return *this;
    }

    Image(Image&& image) noexcept :
        m_termBuffers(image.m_termBuffers),
        m_header(image.m_header),
        m_subscriberPosition(image.m_subscriberPosition),
        m_logBuffers(std::move(image.m_logBuffers)),
        m_sourceIdentity(std::move(image.m_sourceIdentity)),
        m_isClosed(image.isClosed()),
        m_exceptionHandler(std::move(image.m_exceptionHandler)),
        m_correlationId(image.m_correlationId),
        m_subscriptionRegistrationId(image.m_subscriptionRegistrationId),
        m_joinPosition(image.m_joinPosition),
        m_finalPosition(image.m_finalPosition),
        m_sessionId(image.m_sessionId),
        m_termLengthMask(image.m_termLengthMask),
        m_positionBitsToShift(image.m_positionBitsToShift),
        m_isEos(image.m_isEos)
    {
    }

    Image& operator = (Image&& image) noexcept
    {
        m_termBuffers = image.m_termBuffers;
        m_header = image.m_header;
        m_subscriberPosition = image.m_subscriberPosition;
        m_logBuffers = std::move(image.m_logBuffers);
        m_sourceIdentity = std::move(image.m_sourceIdentity);
        m_isClosed = image.isClosed();
        m_exceptionHandler = std::move(image.m_exceptionHandler);
        m_correlationId = image.m_correlationId;
        m_subscriptionRegistrationId = image.m_subscriptionRegistrationId;
        m_joinPosition = image.m_joinPosition;
        m_finalPosition = image.m_finalPosition;
        m_sessionId = image.m_sessionId;
        m_termLengthMask = image.m_termLengthMask;
        m_positionBitsToShift = image.m_positionBitsToShift;
        m_isEos = image.m_isEos;

        return *this;
    }

    /**
     * Get the length in bytes for each term partition in the log buffer.
     *
     * @return the length in bytes for each term partition in the log buffer.
     */
    inline std::int32_t termBufferLength() const
    {
        return m_termBuffers[0].capacity();
    }

    /**
     * Number of bits to right shift a position to get a term count for how far the stream has progressed.
     *
     * @return of bits to right shift a position to get a term count for how far the stream has progressed.
     */
    inline std::int32_t positionBitsToShift() const
    {
        return m_positionBitsToShift;
    }

    /**
     * The sessionId for the steam of messages.
     *
     * @return the sessionId for the steam of messages.
     */
    inline std::int32_t sessionId() const
    {
        return m_sessionId;
    }

    /**
     * The correlationId for identification of the image with the media driver.
     *
     * @return the correlationId for identification of the image with the media driver.
     */
    inline std::int64_t correlationId() const
    {
        return m_correlationId;
    }

    /**
     * The registrationId for the Subscription of the Image.
     *
     * @return the registrationId for the Subscription of the Image.
     */
    inline std::int64_t subscriptionRegistrationId() const
    {
        return m_subscriptionRegistrationId;
    }

    /**
     * The position at which this stream was joined.
     *
     * @return the position at which this stream was joined.
     */
    inline std::int64_t joinPosition() const
    {
        return m_joinPosition;
    }

    /**
     * The initial term at which the stream started for this session.
     *
     * @return the initial term id.
     */
    inline std::int32_t initialTermId() const
    {
        return m_header.initialTermId();
    }

    /**
     * The source identity of the sending publisher as an abstract concept appropriate for the media.
     *
     * @return source identity of the sending publisher as an abstract concept appropriate for the media.
     */
    inline std::string sourceIdentity() const
    {
        return m_sourceIdentity;
    }

    /**
     * Has this object been closed and should no longer be used?
     *
     * @return true if it has been closed otherwise false.
     */
    inline bool isClosed() const
    {
        return std::atomic_load_explicit(&m_isClosed, std::memory_order_acquire);
    }

    /**
     * The position this Image has been consumed to by the subscriber.
     *
     * @return the position this Image has been consumed to by the subscriber or CLOSED if closed
     */
    inline std::int64_t position() const
    {
        if (isClosed())
        {
            return m_finalPosition;
        }

        return m_subscriberPosition.get();
    }

    /**
     * Get the counter id used to represent the subscriber position.
     *
     * @return the counter id used to represent the subscriber position.
     */
    inline std::int32_t subscriberPositionId() const
    {
        return m_subscriberPosition.id();
    }

    /**
     * Set the subscriber position for this Image to indicate where it has been consumed to.
     *
     * @param newPosition for the consumption point.
     */
    inline void position(std::int64_t newPosition)
    {
        if (!isClosed())
        {
            validatePosition(newPosition);
            m_subscriberPosition.setOrdered(newPosition);
        }
    }

    /**
     * Is the current consumed position at the end of the stream?
     *
     * @return true if at the end of the stream or false if not.
     */
    inline bool isEndOfStream() const
    {
        if (isClosed())
        {
            return m_isEos;
        }

        return m_subscriberPosition.get() >=
            LogBufferDescriptor::endOfStreamPosition(m_logBuffers->atomicBuffer(
                LogBufferDescriptor::LOG_META_DATA_SECTION_INDEX));
    }

    /**
     * Poll for new messages in a stream. If new messages are found beyond the last consumed position then they
     * will be delivered via the fragment_handler_t up to a limited number of fragments as specified.
     *
     * @param fragmentHandler to which messages are delivered.
     * @param fragmentLimit   for the number of fragments to be consumed during one polling operation.
     * @return the number of fragments that have been consumed.
     *
     * @see fragment_handler_t
     */
    template <typename F>
    inline int poll(F&& fragmentHandler, int fragmentLimit)
    {
        int result = 0;

        if (!isClosed())
        {
            const std::int64_t position = m_subscriberPosition.get();
            const std::int32_t termOffset = static_cast<std::int32_t>(position & m_termLengthMask);
            const int index = LogBufferDescriptor::indexByPosition(position, m_positionBitsToShift);
            assert(index >= 0 && index < LogBufferDescriptor::PARTITION_COUNT);
            AtomicBuffer &termBuffer = m_termBuffers[index];
            TermReader::ReadOutcome readOutcome{};

            TermReader::read(
                readOutcome, termBuffer, termOffset, fragmentHandler, fragmentLimit, m_header, m_exceptionHandler);

            const std::int64_t newPosition = position + (readOutcome.offset - termOffset);
            if (newPosition > position)
            {
                m_subscriberPosition.setOrdered(newPosition);
            }

            result = readOutcome.fragmentsRead;
        }

        return result;
    }

    /**
     * Poll for new messages in a stream. If new messages are found beyond the last consumed position then they
     * will be delivered to the controlled_poll_fragment_handler_t up to a limited number of fragments as specified.
     *
     * To assemble messages that span multiple fragments then use ControlledFragmentAssembler.
     *
     * @param fragmentHandler to which message fragments are delivered.
     * @param fragmentLimit   for the number of fragments to be consumed during one polling operation.
     * @return the number of fragments that have been consumed.
     *
     * @see controlled_poll_fragment_handler_t
     */
    template <typename F>
    inline int controlledPoll(F&& fragmentHandler, int fragmentLimit)
    {
        int result = 0;

        if (!isClosed())
        {
            int fragmentsRead = 0;
            std::int64_t initialPosition = m_subscriberPosition.get();
            std::int32_t initialOffset = static_cast<std::int32_t>(initialPosition & m_termLengthMask);
            const int index = LogBufferDescriptor::indexByPosition(initialPosition, m_positionBitsToShift);
            assert(index >= 0 && index < LogBufferDescriptor::PARTITION_COUNT);
            AtomicBuffer &termBuffer = m_termBuffers[index];
            std::int32_t resultingOffset = initialOffset;
            const util::index_t capacity = termBuffer.capacity();

            m_header.buffer(termBuffer);

            try
            {
                while (fragmentsRead < fragmentLimit && resultingOffset < capacity)
                {
                    const std::int32_t length = FrameDescriptor::frameLengthVolatile(termBuffer, resultingOffset);
                    if (length <= 0)
                    {
                        break;
                    }

                    const std::int32_t frameOffset = resultingOffset;
                    const std::int32_t alignedLength = util::BitUtil::align(length, FrameDescriptor::FRAME_ALIGNMENT);
                    resultingOffset += alignedLength;

                    if (FrameDescriptor::isPaddingFrame(termBuffer, frameOffset))
                    {
                        continue;
                    }

                    m_header.offset(frameOffset);

                    const ControlledPollAction action = fragmentHandler(
                        termBuffer,
                        frameOffset + DataFrameHeader::LENGTH,
                        length - DataFrameHeader::LENGTH,
                        m_header);

                    if (ControlledPollAction::ABORT == action)
                    {
                        resultingOffset -= alignedLength;
                        break;
                    }

                    ++fragmentsRead;

                    if (ControlledPollAction::BREAK == action)
                    {
                        break;
                    }
                    else if (ControlledPollAction::COMMIT == action)
                    {
                        initialPosition += (resultingOffset - initialOffset);
                        initialOffset = resultingOffset;
                        m_subscriberPosition.setOrdered(initialPosition);
                    }
                }
            }
            catch (const std::exception& ex)
            {
                m_exceptionHandler(ex);
            }

            const std::int64_t resultingPosition = initialPosition + (resultingOffset - initialOffset);
            if (resultingPosition > initialPosition)
            {
                m_subscriberPosition.setOrdered(resultingPosition);
            }

            result = fragmentsRead;
        }

        return result;
    }

    /**
     * Poll for new messages in a stream. If new messages are found beyond the last consumed position then they
     * will be delivered to the controlled_poll_fragment_handler_t up to a limited number of fragments as specified
     * or the maximum position specified.
     *
     * To assemble messages that span multiple fragments then use ControlledFragmentAssembler.
     *
     * @param fragmentHandler to which message fragments are delivered.
     * @param maxPosition     to consume messages up to.
     * @param fragmentLimit   for the number of fragments to be consumed during one polling operation.
     * @return the number of fragments that have been consumed.
     * @see controlled_poll_fragment_handler_t
     */
    template <typename F>
    inline int boundedControlledPoll(F&& fragmentHandler, std::int64_t maxPosition, int fragmentLimit)
    {
        int result = 0;

        if (!isClosed())
        {
            int fragmentsRead = 0;
            std::int64_t initialPosition = m_subscriberPosition.get();
            std::int32_t initialOffset = static_cast<std::int32_t>(initialPosition & m_termLengthMask);
            const int index = LogBufferDescriptor::indexByPosition(initialPosition, m_positionBitsToShift);
            assert(index >= 0 && index < LogBufferDescriptor::PARTITION_COUNT);
            AtomicBuffer &termBuffer = m_termBuffers[index];
            std::int32_t resultingOffset = initialOffset;
            const std::int64_t capacity = termBuffer.capacity();
            const std::int32_t endOffset =
                static_cast<std::int32_t>(std::min(capacity, (maxPosition - initialPosition) + initialOffset));

            m_header.buffer(termBuffer);

            try
            {
                while (fragmentsRead < fragmentLimit && resultingOffset < endOffset)
                {
                    const std::int32_t length = FrameDescriptor::frameLengthVolatile(termBuffer, resultingOffset);
                    if (length <= 0)
                    {
                        break;
                    }

                    const std::int32_t frameOffset = resultingOffset;
                    const std::int32_t alignedLength = util::BitUtil::align(length, FrameDescriptor::FRAME_ALIGNMENT);
                    resultingOffset += alignedLength;

                    if (FrameDescriptor::isPaddingFrame(termBuffer, frameOffset))
                    {
                        continue;
                    }

                    m_header.offset(frameOffset);

                    const ControlledPollAction action = fragmentHandler(
                        termBuffer,
                        frameOffset + DataFrameHeader::LENGTH,
                        length - DataFrameHeader::LENGTH,
                        m_header);

                    if (ControlledPollAction::ABORT == action)
                    {
                        resultingOffset -= alignedLength;
                        break;
                    }

                    ++fragmentsRead;

                    if (ControlledPollAction::BREAK == action)
                    {
                        break;
                    }
                    else if (ControlledPollAction::COMMIT == action)
                    {
                        initialPosition += (resultingOffset - initialOffset);
                        initialOffset = resultingOffset;
                        m_subscriberPosition.setOrdered(initialPosition);
                    }
                }
            }
            catch (const std::exception& ex)
            {
                m_exceptionHandler(ex);
            }

            const std::int64_t resultingPosition = initialPosition + (resultingOffset - initialOffset);
            if (resultingPosition > initialPosition)
            {
                m_subscriberPosition.setOrdered(resultingPosition);
            }

            result = fragmentsRead;
        }

        return result;
    }

    /**
     * Peek for new messages in a stream by scanning forward from an initial position. If new messages are found then
     * they will be delivered to the controlled_poll_fragment_handler_t up to a limited position.
     * <p>
     * To assemble messages that span multiple fragments then use ControlledFragmentAssembler. Scans must also
     * start at the beginning of a message so that the assembler is reset.
     *
     * @param initialPosition from which to peek forward.
     * @param fragmentHandler to which message fragments are delivered.
     * @param limitPosition   up to which can be scanned.
     * @return the resulting position after the scan terminates which is a complete message.
     * @see controlled_poll_fragment_handler_t
     */
    template <typename F>
    inline std::int64_t controlledPeek(std::int64_t initialPosition, F&& fragmentHandler, std::int64_t limitPosition)
    {
        std::int64_t resultingPosition = initialPosition;

        if (!isClosed())
        {
            validatePosition(initialPosition);

            std::int32_t initialOffset = static_cast<std::int32_t>(initialPosition & m_termLengthMask);
            std::int32_t offset = initialOffset;
            std::int64_t position = initialPosition;
            const int index = LogBufferDescriptor::indexByPosition(initialPosition, m_positionBitsToShift);
            assert(index >= 0 && index < LogBufferDescriptor::PARTITION_COUNT);
            AtomicBuffer &termBuffer = m_termBuffers[index];
            const util::index_t capacity = termBuffer.capacity();

            m_header.buffer(termBuffer);

            try
            {
                while (position < limitPosition && offset < capacity)
                {
                    const std::int32_t length = FrameDescriptor::frameLengthVolatile(termBuffer, offset);
                    if (length <= 0)
                    {
                        break;
                    }

                    const std::int32_t frameOffset = offset;
                    const std::int32_t alignedLength = util::BitUtil::align(length, FrameDescriptor::FRAME_ALIGNMENT);
                    offset += alignedLength;

                    if (FrameDescriptor::isPaddingFrame(termBuffer, frameOffset))
                    {
                        position += (offset - initialOffset);
                        initialOffset = offset;
                        resultingPosition = position;
                        continue;
                    }

                    m_header.offset(frameOffset);

                    const ControlledPollAction action = fragmentHandler(
                        termBuffer,
                        frameOffset + DataFrameHeader::LENGTH,
                        length - DataFrameHeader::LENGTH,
                        m_header);

                    if (ControlledPollAction::ABORT == action)
                    {
                        break;
                    }

                    position += (offset - initialOffset);
                    initialOffset = offset;

                    if (m_header.flags() & FrameDescriptor::END_FRAG)
                    {
                        resultingPosition = position;
                    }

                    if (ControlledPollAction::BREAK == action)
                    {
                        break;
                    }
                }
            }
            catch (const std::exception& ex)
            {
                m_exceptionHandler(ex);
            }
        }

        return resultingPosition;
    }

    /**
     * Poll for new messages in a stream. If new messages are found beyond the last consumed position then they
     * will be delivered via the block_handler_t up to a limited number of bytes.
     *
     * A scan will terminate if a padding frame is encountered. If first frame in a scan is padding then a block
     * for the padding is notified. If the padding comes after the first frame in a scan then the scan terminates
     * at the offset the padding frame begins. Padding frames are delivered singularly in a block.
     *
     * Padding frames may be for a greater range than the limit offset but only the header needs to be valid so
     * relevant length of the frame is sizeof DataHeaderDefn.
     *
     * @param blockHandler     to which block is delivered.
     * @param blockLengthLimit up to which a block may be in length.
     * @return the number of bytes that have been consumed.
     *
     * @see block_handler_t
     */
    template <typename F>
    inline int blockPoll(F&& blockHandler, int blockLengthLimit)
    {
        int result = 0;

        if (!isClosed())
        {
            const std::int64_t position = m_subscriberPosition.get();
            const std::int32_t termOffset = static_cast<std::int32_t>(position & m_termLengthMask);
            const int index = LogBufferDescriptor::indexByPosition(position, m_positionBitsToShift);
            assert(index >= 0 && index < LogBufferDescriptor::PARTITION_COUNT);
            AtomicBuffer &termBuffer = m_termBuffers[index];
            const std::int32_t limitOffset = std::min(termOffset + blockLengthLimit, termBuffer.capacity());
            const std::int32_t resultingOffset = TermBlockScanner::scan(termBuffer, termOffset, limitOffset);
            const std::int32_t length = resultingOffset - termOffset;

            if (resultingOffset > termOffset)
            {
                try
                {
                    const std::int32_t termId = termBuffer.getInt32(termOffset + DataFrameHeader::TERM_ID_FIELD_OFFSET);
                    blockHandler(termBuffer, termOffset, length, m_sessionId, termId);
                }
                catch (const std::exception& ex)
                {
                    m_exceptionHandler(ex);
                }

                m_subscriberPosition.setOrdered(position + length);
            }

            result = length;
        }

        return result;
    }

    std::shared_ptr<LogBuffers> logBuffers()
    {
        return m_logBuffers;
    }

    /// @cond HIDDEN_SYMBOLS
    inline void close()
    {
        if (!isClosed())
        {
            m_finalPosition = m_subscriberPosition.getVolatile();
            m_isEos = m_finalPosition >= LogBufferDescriptor::endOfStreamPosition(
                m_logBuffers->atomicBuffer(LogBufferDescriptor::LOG_META_DATA_SECTION_INDEX));
            std::atomic_store_explicit(&m_isClosed, true, std::memory_order_release);
        }
    }
    /// @endcond

private:
    std::array<AtomicBuffer, LogBufferDescriptor::PARTITION_COUNT> m_termBuffers;
    Header m_header;
    Position<UnsafeBufferPosition> m_subscriberPosition;
    std::shared_ptr<LogBuffers> m_logBuffers;
    std::string m_sourceIdentity;
    std::atomic<bool> m_isClosed;
    exception_handler_t m_exceptionHandler;

    std::int64_t m_correlationId;
    std::int64_t m_subscriptionRegistrationId;
    std::int64_t m_joinPosition;
    std::int64_t m_finalPosition;
    std::int32_t m_sessionId;
    std::int32_t m_termLengthMask;
    std::int32_t m_positionBitsToShift;
    bool m_isEos;

    void validatePosition(std::int64_t newPosition)
    {
        const std::int64_t position = m_subscriberPosition.get();
        const std::int64_t limitPosition = (position - (position & m_termLengthMask)) + m_termLengthMask + 1;

        if (newPosition < position || newPosition > limitPosition)
        {
            throw util::IllegalArgumentException(
                std::to_string(newPosition) + " newPosition out of range " +
                std::to_string(position) + " - " + std::to_string(limitPosition),
                SOURCEINFO);
        }

        if (0 != (newPosition & (FrameDescriptor::FRAME_ALIGNMENT - 1)))
        {
            throw util::IllegalArgumentException(
                std::to_string(newPosition) + " newPosition not aligned to FRAME_ALIGNMENT",
                SOURCEINFO);
        }
    }
};

}

#endif
