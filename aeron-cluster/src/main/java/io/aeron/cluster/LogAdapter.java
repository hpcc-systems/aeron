/*
 *  Copyright 2014-2019 Real Logic Ltd.
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
package io.aeron.cluster;

import io.aeron.Image;
import io.aeron.ImageControlledFragmentAssembler;
import io.aeron.cluster.client.ClusterException;
import io.aeron.cluster.codecs.*;
import io.aeron.logbuffer.ControlledFragmentHandler;
import io.aeron.logbuffer.Header;
import org.agrona.DirectBuffer;

import static io.aeron.cluster.client.AeronCluster.SESSION_HEADER_LENGTH;

final class LogAdapter implements ControlledFragmentHandler, AutoCloseable
{
    private static final int FRAGMENT_LIMIT = 100;

    private final ImageControlledFragmentAssembler fragmentAssembler = new ImageControlledFragmentAssembler(this);
    private final Image image;
    private final ConsensusModuleAgent consensusModuleAgent;
    private final MessageHeaderDecoder messageHeaderDecoder = new MessageHeaderDecoder();
    private final SessionOpenEventDecoder sessionOpenEventDecoder = new SessionOpenEventDecoder();
    private final SessionCloseEventDecoder sessionCloseEventDecoder = new SessionCloseEventDecoder();
    private final SessionMessageHeaderDecoder sessionHeaderDecoder = new SessionMessageHeaderDecoder();
    private final TimerEventDecoder timerEventDecoder = new TimerEventDecoder();
    private final ClusterActionRequestDecoder clusterActionRequestDecoder = new ClusterActionRequestDecoder();
    private final NewLeadershipTermEventDecoder newLeadershipTermEventDecoder = new NewLeadershipTermEventDecoder();
    private final MembershipChangeEventDecoder membershipChangeEventDecoder = new MembershipChangeEventDecoder();

    LogAdapter(final Image image, final ConsensusModuleAgent consensusModuleAgent)
    {
        this.image = image;
        this.consensusModuleAgent = consensusModuleAgent;
    }

    public void close()
    {
        image.subscription().close();
    }

    long position()
    {
        return image.position();
    }

    int poll(final long boundPosition)
    {
        return image.boundedControlledPoll(fragmentAssembler, boundPosition, FRAGMENT_LIMIT);
    }

    boolean isImageClosed()
    {
        return image.isClosed();
    }

    Image image()
    {
        return image;
    }

    void removeDestination(final String destination)
    {
        if (null != image)
        {
            image.subscription().removeDestination(destination);
        }
    }

    @SuppressWarnings("MethodLength")
    public Action onFragment(final DirectBuffer buffer, final int offset, final int length, final Header header)
    {
        messageHeaderDecoder.wrap(buffer, offset);

        final int schemaId = messageHeaderDecoder.schemaId();
        if (schemaId != MessageHeaderDecoder.SCHEMA_ID)
        {
            throw new ClusterException("expected schemaId=" + MessageHeaderDecoder.SCHEMA_ID + ", actual=" + schemaId);
        }

        final int templateId = messageHeaderDecoder.templateId();
        if (templateId == SessionMessageHeaderDecoder.TEMPLATE_ID)
        {
            sessionHeaderDecoder.wrap(
                buffer,
                offset + MessageHeaderDecoder.ENCODED_LENGTH,
                messageHeaderDecoder.blockLength(),
                messageHeaderDecoder.version());

            consensusModuleAgent.onReplaySessionMessage(
                sessionHeaderDecoder.clusterSessionId(),
                sessionHeaderDecoder.timestamp(),
                buffer,
                offset + SESSION_HEADER_LENGTH,
                length - SESSION_HEADER_LENGTH,
                header);

            return Action.CONTINUE;
        }

        switch (templateId)
        {
            case TimerEventDecoder.TEMPLATE_ID:
                timerEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onReplayTimerEvent(
                    timerEventDecoder.correlationId(),
                    timerEventDecoder.timestamp());
                break;

            case SessionOpenEventDecoder.TEMPLATE_ID:
                sessionOpenEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onReplaySessionOpen(
                    header.position(),
                    sessionOpenEventDecoder.correlationId(),
                    sessionOpenEventDecoder.clusterSessionId(),
                    sessionOpenEventDecoder.timestamp(),
                    sessionOpenEventDecoder.responseStreamId(),
                    sessionOpenEventDecoder.responseChannel());
                break;

            case SessionCloseEventDecoder.TEMPLATE_ID:
                sessionCloseEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onReplaySessionClose(
                    sessionCloseEventDecoder.clusterSessionId(),
                    sessionCloseEventDecoder.timestamp(),
                    sessionCloseEventDecoder.closeReason());
                break;

            case NewLeadershipTermEventDecoder.TEMPLATE_ID:
                newLeadershipTermEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onReplayNewLeadershipTermEvent(
                    newLeadershipTermEventDecoder.leadershipTermId(),
                    newLeadershipTermEventDecoder.logPosition(),
                    newLeadershipTermEventDecoder.timestamp(),
                    newLeadershipTermEventDecoder.termBaseLogPosition(),
                    newLeadershipTermEventDecoder.leaderMemberId(),
                    newLeadershipTermEventDecoder.logSessionId());
                break;

            case MembershipChangeEventDecoder.TEMPLATE_ID:
                membershipChangeEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onMembershipChange(
                    membershipChangeEventDecoder.leadershipTermId(),
                    membershipChangeEventDecoder.logPosition(),
                    membershipChangeEventDecoder.timestamp(),
                    membershipChangeEventDecoder.leaderMemberId(),
                    membershipChangeEventDecoder.clusterSize(),
                    membershipChangeEventDecoder.changeType(),
                    membershipChangeEventDecoder.memberId(),
                    membershipChangeEventDecoder.clusterMembers());
                break;

            case ClusterActionRequestDecoder.TEMPLATE_ID:
                clusterActionRequestDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                consensusModuleAgent.onReplayClusterAction(
                    clusterActionRequestDecoder.leadershipTermId(),
                    clusterActionRequestDecoder.logPosition(),
                    clusterActionRequestDecoder.timestamp(),
                    clusterActionRequestDecoder.action());
                return Action.BREAK;
        }

        return Action.CONTINUE;
    }
}
