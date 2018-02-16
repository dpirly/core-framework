/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of REDHAWK bulkioInterfaces.
 *
 * REDHAWK bulkioInterfaces is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * REDHAWK bulkioInterfaces is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#include "InPortTest.h"

class SriListener {
public:
    void updateSRI(BULKIO::StreamSRI& sri)
    {
        this->sri.push_back(sri);
    }

    std::vector<BULKIO::StreamSRI> sri;
};

template <class Port>
void InPortTest<Port>::testLegacyAPI()
{
    // Test for methods that are technically still supported, but discouraged
    port->enableStats(false);

    port->block();

    port->unblock();
}

template <class Port>
void InPortTest<Port>::testGetPacket()
{
    // Port queue starts empty
    boost::scoped_ptr<PacketType> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(!packet);

    const char* stream_id = "test_get_packet";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);

    BULKIO::PrecisionUTCTime ts = bulkio::time::utils::now();
    this->_pushTestPacket(50, ts, false, stream_id);

    // Check result of getPacket
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT_EQUAL((size_t) 50, packet->dataBuffer.size());
    CPPUNIT_ASSERT(!packet->EOS);
    CPPUNIT_ASSERT_EQUAL(std::string(stream_id), packet->streamID);
    CPPUNIT_ASSERT(bulkio::sri::DefaultComparator(sri, packet->SRI));
    CPPUNIT_ASSERT(packet->sriChanged);
    CPPUNIT_ASSERT(!packet->inputQueueFlushed);

    // No packet, should return null
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(!packet);

    // Change mode to complex and push another packet with EOS set
    sri.mode = 1;
    port->pushSRI(sri);
    this->_pushTestPacket(100, ts, true, sri.streamID);
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT_EQUAL((size_t) 100, packet->dataBuffer.size());
    CPPUNIT_ASSERT(packet->EOS);
    CPPUNIT_ASSERT(packet->sriChanged);
    CPPUNIT_ASSERT_EQUAL(1, (int) packet->SRI.mode);
}

template <class Port>
void InPortTest<Port>::testActiveSRIs()
{
    BULKIO::StreamSRISequence_var active_sris = port->activeSRIs();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 0, active_sris->length());

    // Push a new SRI, and make sure that it is immediately visible and
    // correct in activeSRIs
    const char* stream_id_1 = "active_sri_1";
    BULKIO::StreamSRI sri_1 = bulkio::sri::create(stream_id_1);
    port->pushSRI(sri_1);
    active_sris = port->activeSRIs();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 1, active_sris->length());
    CPPUNIT_ASSERT(bulkio::sri::DefaultComparator(active_sris[0], sri_1));

    // Push a second SRI, and make sure that activeSRIs is up-to-date
    const char* stream_id_2 = "active_sri_2";
    BULKIO::StreamSRI sri_2 = bulkio::sri::create(stream_id_2);
    port->pushSRI(sri_2);
    active_sris = port->activeSRIs();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 2, active_sris->length());
    for (size_t index = 0; index < active_sris->length(); ++index) {
        const std::string current_id(active_sris[index].streamID);
        if (current_id == stream_id_2) {
            CPPUNIT_ASSERT(bulkio::sri::DefaultComparator(active_sris[index], sri_2));
        } else if (current_id != stream_id_1) {
            CPPUNIT_FAIL("unexpected SRI '" + current_id + "'");
        }
    }

    // Push an end-of-stream, retrieve the packet, and verify that the
    // stream is no longer in activeSRIs
    this->_pushTestPacket(0, bulkio::time::utils::notSet(), true, stream_id_1);
    boost::scoped_ptr<PacketType> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->EOS);
    active_sris = port->activeSRIs();
    CPPUNIT_ASSERT_EQUAL((CORBA::ULong) 1, active_sris->length());
    CPPUNIT_ASSERT_EQUAL(std::string(stream_id_2), std::string(active_sris[0].streamID));
}

template <class Port>
void InPortTest<Port>::testQueueDepth()
{
    const char* stream_id = "test_state";

    // The port had better start with an empty queue
    CPPUNIT_ASSERT_EQUAL(0, port->getCurrentQueueDepth());

    // Use a non-blocking stream to allow queue flushing
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    sri.blocking = false;
    port->pushSRI(sri);

    // Push some test packets, the queue should start growing
    for (int ii = 0; ii < 4; ii++) {
        this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    }
    CPPUNIT_ASSERT_EQUAL(4, port->getCurrentQueueDepth());

    // Read a packet and make sure the current depth drops
    boost::scoped_ptr<PacketType> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT_EQUAL(3, port->getCurrentQueueDepth());

    // Reduce the max queue size and push another packet, causing a flush
    port->setMaxQueueDepth(3);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    CPPUNIT_ASSERT_EQUAL(1, port->getCurrentQueueDepth());

    // Read the packet and make sure the flush is reported
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->inputQueueFlushed);

    // One more packet, should not report a flush
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(!packet->inputQueueFlushed);
}

template <class Port>
void InPortTest<Port>::testState()
{
    const char* stream_id = "test_state";

    // Port starts out idle
    CPPUNIT_ASSERT_EQUAL(BULKIO::IDLE, port->state());

    // Push one test packet, state goes to active
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    CPPUNIT_ASSERT_EQUAL(BULKIO::ACTIVE, port->state());

    // Full queue should report busy
    port->setMaxQueueDepth(2);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    CPPUNIT_ASSERT_EQUAL(BULKIO::BUSY, port->state());

    // Drop below max, back to active
    boost::scoped_ptr<PacketType> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT_EQUAL(BULKIO::ACTIVE, port->state());

    // Empty queue, back to idle
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT_EQUAL(BULKIO::IDLE, port->state());
}

template <class Port>
void InPortTest<Port>::testSriChanged()
{
    const char* stream_id = "sri_changed";

    SriListener listener;
    port->setNewStreamListener(&listener, &SriListener::updateSRI);
    CPPUNIT_ASSERT(listener.sri.empty());

    // Create a default SRI and push it, which should trigger the callback
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());
    CPPUNIT_ASSERT_EQUAL(std::string(stream_id), std::string(listener.sri.back().streamID));

    // SRI should report changed for first packet
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    boost::scoped_ptr<typename Port::dataTransfer> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->sriChanged);
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());

    // No SRI change for second packet
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(!packet->sriChanged);

    // Change the SRI, should flag the packet
    sri.mode = 1;
    port->pushSRI(sri);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->sriChanged);
}

template <class Port>
void InPortTest<Port>::testSriChangedFlush()
{
    const char* stream_id = "sri_changed_flush";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);

    // Reduce the queue size so we can force a flush
    port->setMaxQueueDepth(2);

    // Push a packet, change the SRI, and push two more packets so that the
    // packet with the associated SRI change gets flushed
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    sri.xdelta = 0.5;
    port->pushSRI(sri);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);
    this->_pushTestPacket(1, bulkio::time::utils::now(), false, stream_id);

    // Get the last packet and verify that the queue has flushed, and the SRI
    // change is still reported
    boost::scoped_ptr<typename Port::dataTransfer> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->inputQueueFlushed);
    CPPUNIT_ASSERT(packet->sriChanged);
}

template <class Port>
void InPortTest<Port>::testSriChangedInvalidStream()
{
    const char* stream_id = "invalid_stream";

    // Turn off the port's logging to avoid dumping a warning to the screen
    port->getLogger()->setLevel(rh_logger::Level::getOff());

    SriListener listener;
    port->setNewStreamListener(&listener, &SriListener::updateSRI);
    CPPUNIT_ASSERT(listener.sri.empty());

    // Push data without an SRI to check that the sriChanged flag is still set
    // and the SRI callback gets called
    this->_pushTestPacket(0, BULKIO::PrecisionUTCTime(), false, stream_id);
    boost::scoped_ptr<typename Port::dataTransfer> packet;
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(packet->sriChanged);
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());
    CPPUNIT_ASSERT_EQUAL(std::string(stream_id), std::string(listener.sri.back().streamID));

    // Push again to the same stream ID; sriChanged should now be false and the
    // SRI callback should not get called
    this->_pushTestPacket(0, BULKIO::PrecisionUTCTime(), false, stream_id);
    packet.reset(port->getPacket(bulkio::Const::NON_BLOCKING));
    CPPUNIT_ASSERT(packet);
    CPPUNIT_ASSERT(!(packet->sriChanged));
    CPPUNIT_ASSERT_EQUAL((size_t) 1, listener.sri.size());
}

template <class Port>
void InPortTest<Port>::testStatistics()
{
    // Push a packet of data to trigger meaningful statistics
    const char* stream_id = "port_stats";
    BULKIO::StreamSRI sri = bulkio::sri::create(stream_id);
    port->pushSRI(sri);
    this->_pushTestPacket(1024, bulkio::time::utils::now(), false, stream_id);

    // Check that the statistics report the right element size
    BULKIO::PortStatistics_var stats = port->statistics();
    CPPUNIT_ASSERT(stats->elementsPerSecond > 0.0);
    size_t bits_per_element = round(stats->bitsPerSecond / stats->elementsPerSecond);
    const size_t expected_bits = bulkio::NativeTraits<CorbaType>::bits;
    CPPUNIT_ASSERT_EQUAL_MESSAGE("Incorrect bits per element", expected_bits, bits_per_element);
}

#define CREATE_TEST(x)                                                 \
    class In##x##PortTest : public InPortTest<bulkio::In##x##Port>      \
    {                                                                   \
        CPPUNIT_TEST_SUB_SUITE(In##x##PortTest, InPortTest<bulkio::In##x##Port>); \
        CPPUNIT_TEST_SUITE_END();                                       \
    };                                                                  \
    CPPUNIT_TEST_SUITE_REGISTRATION(In##x##PortTest);

CREATE_TEST(Octet);
CREATE_TEST(Char);
CREATE_TEST(Short);
CREATE_TEST(UShort);
CREATE_TEST(Long);
CREATE_TEST(ULong);
CREATE_TEST(LongLong);
CREATE_TEST(ULongLong);
CREATE_TEST(Float);
CREATE_TEST(Double);
CREATE_TEST(Bit);
CREATE_TEST(XML);
CREATE_TEST(File);
