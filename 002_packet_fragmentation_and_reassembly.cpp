/*
    Example source code for "Packet Fragmentation and Reassembly"

    http://gafferongames.com/building-a-game-network-protocol/packet-fragmentation-and-reassembly

    This software is in the public domain. Where that dedication is not recognized, 
    you are granted a perpetual, irrevocable license to copy, distribute, and modify this file as you see fit.
*/

#define PROTOCOL2_IMPLEMENTATION

#include "protocol2.h"
#include <stdio.h>
#include <time.h>

const int PacketBufferSize = 256;                       // size of packet buffer, eg. number of historical packets for which we can buffer fragments
const int MaxFragmentSize = 1024;                       // maximum size of a packet fragment
const int MaxFragmentsPerPacket = 256;                  // maximum number of fragments per-packet
const int MaxBufferedFragments = 256;                   // maximum number of buffered fragments (in total) per-packet buffer

const int MaxPacketSize = MaxFragmentSize * MaxFragmentsPerPacket;

const int NumIterations = 32;                               

const uint32_t ProtocolId = 0x55667788;

const int PacketFragmentHeaderBytes = 16;

enum TestPacketTypes
{
    PACKET_FRAGMENT = 0,                    // IMPORTANT: packet type 0 indicates a packet fragment

    TEST_PACKET_A,
    TEST_PACKET_B,
    TEST_PACKET_C,

    TEST_PACKET_NUM_TYPES
};

// fragment packet on-the-wire format:
// [crc32] (32 bits) | [sequence] (16 bits) | [packet type 0] (# of bits depends on number of packet types) 
// [fragment id] (8 bits) | [num fragments] (8 bits) | (pad zero bits to nearest byte) | <fragment data>

struct FragmentPacket : public protocol2::Object
{
    // input/output

    int fragmentSize;                   // set as input on serialize write. output on serialize read (inferred from size of packet)

    // serialized data

    uint32_t crc32;
    uint16_t sequence;
    int packetType;
    uint8_t fragmentId;
    uint8_t numFragments;
    uint8_t fragmentData[MaxFragmentSize];

    PROTOCOL2_SERIALIZE_OBJECT( stream )
    {
        serialize_bits( stream, crc32, 32 );
        serialize_bits( stream, sequence, 16 );

        packetType = 0;
        serialize_int( stream, packetType, 0, TEST_PACKET_NUM_TYPES - 1 );
        if ( packetType != 0 )
            return true;

        serialize_bits( stream, fragmentId, 8 );
        serialize_bits( stream, numFragments, 8 );

        serialize_align( stream );

        printf( "bytes processed: %d\n", stream.GetBytesProcessed() );

        if ( Stream::IsReading )
        {
            assert( ( stream.GetBitsRemaining() % 8 ) == 0 );
            fragmentSize = stream.GetBitsRemaining() / 8;
            if ( fragmentSize <= 0 || fragmentSize > MaxFragmentSize )
            {
                printf( "fragment size is out of bounds (%d)\n", fragmentSize );
                return false;
            }
        }
        else
        {
            printf( "fragmentSize = %d\n", fragmentSize );
        }

        assert( fragmentSize > 0 );
        assert( fragmentSize <= MaxFragmentSize );

        serialize_bytes( stream, fragmentData, fragmentSize );

        return true;
    }
};

struct PacketData
{
    int size;
    uint8_t *data;
};

struct PacketBufferEntry
{
    uint32_t sequence : 16;                             // packet sequence number
    uint32_t numFragments : 8;                          // number of fragments for this packet
    uint32_t receivedFragments : 8;                     // number of received fragments so far
    int fragmentSize[MaxFragmentsPerPacket];            // size of fragment n in bytes
    uint8_t *fragmentData[MaxFragmentsPerPacket];       // pointer to data for fragment n 
};

struct PacketBuffer
{
    PacketBuffer() { memset( this, 0, sizeof( PacketBuffer ) ); }

    uint16_t currentSequence;                           // sequence number of most recent packet in buffer

    int numFragments;                                   // number of fragments currently buffered

    bool valid[PacketBufferSize];                       // true if there is a valid buffered packet entry at this index

    PacketBufferEntry entries[PacketBufferSize];        // buffered packets in range [ current_sequence - PacketBufferSize + 1, current_sequence ] (modulo 65536)

    /*
        Advance the current sequence for the packet buffer forward.
        This function removes old packet entries older and frees their packets.
    */

    void Advance( uint16_t sequence )
    {
        if ( !protocol2::sequence_greater_than( sequence, currentSequence ) )
            return;

        printf( "advance %d\n", sequence );

        const uint16_t oldestSequence = sequence - PacketBufferSize + 1;

        for ( int i = 0; i < PacketBufferSize; ++i )
        {
            if ( valid[i] )
            {
                if ( protocol2::sequence_less_than( entries[i].sequence, oldestSequence ) )
                {
                    printf( "remove old packet entry %d\n", entries[i].sequence );

                    for ( int j = 0; j < entries[i].numFragments; ++j )
                    {
                        if ( entries[i].fragmentData[j] )
                        {
                            delete [] entries[i].fragmentData[j];
                            assert( numFragments > 0 );
                            numFragments--;
                        }
                    }
                }

                memset( &entries[i], 0, sizeof( PacketBufferEntry ) );

                valid[i] = false;
            }
        }

        currentSequence = sequence;
    }

    /*
        Process packet fragment on receiver side.

        Stores each fragment ready to receive the whole packet once all fragments for that packet are received.

        If any fragment is dropped, fragments are not resent, the whole packet is dropped.

        NOTE: This function is fairly complicated because it must handle all possible cases
        of maliciously constructed packets attempting to overflow and corrupt the packet buffer!
    */

    bool ProcessFragment( const uint8_t *fragmentData, int fragmentSize, uint16_t packetSequence, int fragmentId, int numFragments )
    {
        assert( fragmentData );

        // too many buffered fragments? discard the fragment

        if ( numFragments >= MaxBufferedFragments )
        {
            printf( "error 1\n" );
            return false;
        }

        // fragment size is <= zero? discard the fragment.

        if ( fragmentSize <= 0 )
        {
            printf( "error 2\n" );
            return false;
        }

        // fragment size exceeds max fragment size? discard the fragment.

        if ( fragmentSize > MaxFragmentSize )
        {
            printf( "error 3\n" );
            return false;
        }

        // num fragments outside of range? discard the fragment

        if ( numFragments <= 0 || numFragments > MaxFragmentsPerPacket )
        {
            printf( "error 4\n" );
            return false;
        }

        // fragment index out of range? discard the fragment

        if ( fragmentId < 0 || fragmentId >= numFragments )
        {
            printf( "error 5\n" );
            return false;
        }

        // if this is not the last fragment in the packet and fragment size is not equal to MaxFragmentSize, discard the fragment

        if ( fragmentId != numFragments - 1 && fragmentSize != MaxFragmentSize )
        {
            printf( "error 6\n" );
            return false;
        }

        // packet sequence number wildly out of range from the current sequence? discard the fragment

        if ( protocol2::sequence_difference( packetSequence, currentSequence ) > 10 * 1024 )
        {
            printf( "error 7\n" );
            return false;
        }

        // if the entry exists, but has a different sequence number, discard the fragment

        const int index = packetSequence % PacketBufferSize;

        if ( valid[index] && entries[index].sequence != packetSequence )
        {
            printf( "error 8\n" );
            return false;
        }

        // if the entry does not exist, add an entry for this sequence # and set total fragments

        if ( !valid[index] )
        {
            Advance( packetSequence );
            entries[index].sequence = packetSequence;
            entries[index].numFragments = numFragments;
            assert( entries[index].receivedFragments == 0 );            // IMPORTANT: Should have already been cleared to zeros in "Advance"
            valid[index] = true;
        }

        // at this point the entry must exist and have the same sequence number as the fragment

        assert( valid[index] );
        assert( entries[index].sequence == packetSequence );

        // if the total number fragments is different for this packet vs. the entry, discard the fragment

        if ( numFragments != entries[index].numFragments )
        {
            printf( "error 9\n" );
            return false;
        }

        // if this fragment has already been received, ignore it because it must have come from a duplicate packet

        assert( fragmentId < numFragments );
        assert( fragmentId < MaxFragmentsPerPacket );
        assert( numFragments <= MaxFragmentsPerPacket );

        if ( entries[index].fragmentSize[fragmentId] )
        {
            printf( "error 10\n" );
            return false;
        }

        // add the fragment to the packet buffer

        printf( "added fragment %d/%d of packet %d to buffer\n", fragmentId, numFragments, packetSequence );

        assert( fragmentSize > 0 );
        assert( fragmentSize <= MaxFragmentSize );

        entries[index].fragmentSize[fragmentId] = fragmentSize;
        entries[index].fragmentData[fragmentId] = new uint8_t[fragmentSize];
        memcpy( entries[index].fragmentData[fragmentId], fragmentData, fragmentSize );
        entries[index].receivedFragments++;

        assert( entries[index].receivedFragments <= entries[index].numFragments );

        numFragments++;

        return true;
    }

    bool ProcessPacket( const uint8_t *data, int size )
    {
        protocol2::ReadStream stream( data, size );

        FragmentPacket fragmentPacket;
        
        if ( !fragmentPacket.SerializeRead( stream ) )
        {
            printf( "fragment packet failed to serialize\n" );
            return false;
        }

        uint32_t protocolId = protocol2::host_to_network( ProtocolId );
        uint32_t crc32 = protocol2::calculate_crc32( (const uint8_t*) &protocolId, 4 );
        uint32_t zero = 0;
        crc32 = protocol2::calculate_crc32( (const uint8_t*) &zero, 4, crc32 );
        crc32 = protocol2::calculate_crc32( data + 4, size - 4, crc32 );

        if ( crc32 != fragmentPacket.crc32 )
        {
            printf( "crc32 mismatch: expected %x, got %x\n", crc32, fragmentPacket.crc32 );
            return false;
        }

        if ( fragmentPacket.packetType == 0 )
        {
            printf( "process fragment %d/%d of packet %d\n", fragmentPacket.fragmentId, fragmentPacket.numFragments, fragmentPacket.sequence );

            return ProcessFragment( data + PacketFragmentHeaderBytes, fragmentPacket.fragmentSize, fragmentPacket.sequence, fragmentPacket.fragmentId, fragmentPacket.numFragments );
        }
        else
        {
            printf( "process regular packet %d\n", fragmentPacket.sequence );

            return ProcessFragment( data, size, fragmentPacket.sequence, 0, 1 );
        }

        return true;
    }

    void ReceivePackets( int & numPackets, PacketData packetData[] )
    {
        numPackets = 0;

        const uint16_t oldestSequence = currentSequence - PacketBufferSize + 1;

        for ( int i = 0; i < PacketBufferSize; ++i )
        {
            const uint16_t sequence = uint16_t( oldestSequence + i );

            const int index = sequence % PacketBufferSize;

            if ( valid[index] && entries[index].sequence == sequence )
            {
                // have all fragments arrived for this packet?

                if ( entries[index].receivedFragments != entries[index].numFragments )
                {
                    printf( "not all packets have arrived for %d [%d/%d]\n", sequence, entries[index].receivedFragments, entries[index].numFragments );
                    continue;
                }

                printf( "received all fragments for packet %d [%d/%d]\n", sequence, entries[index].receivedFragments, entries[index].numFragments );

                // what's the total size of this packet?

                int packetSize = 0;
                for ( int j = 0; j < entries[index].numFragments; ++j )
                {
                    packetSize += entries[index].fragmentSize[j];
                }

                assert( packetSize > 0 );
                assert( packetSize <= MaxPacketSize );

                // allocate a packet to return to the caller

                PacketData & packet = packetData[numPackets++];

                packet.size = packetSize;
                packet.data = new uint8_t[packetSize];

                // reconstruct the packet from the fragments

                uint8_t *dst = packet.data;
                for ( int j = 0; j < entries[index].numFragments; ++j )
                {
                    memcpy( dst, entries[index].fragmentData[i], entries[index].fragmentSize[i] );
                    dst += entries[index].fragmentSize[i];
                }

                // free all fragments

                for ( int j = 0; j < entries[index].numFragments; ++j )
                {
                    delete [] entries[index].fragmentData[j];
                    numFragments--;
                }

                // clear the packet buffer entry

                memset( &entries[index], 0, sizeof( PacketBufferEntry ) );

                valid[index] = false;
            }
        }
    }
};

bool SplitPacketIntoFragments( uint16_t sequence, const uint8_t *packetData, int packetSize, int & numFragments, PacketData fragmentPackets[] )
{
    numFragments = 0;

    assert( packetData );
    assert( packetSize > 0 );
    assert( packetSize < MaxPacketSize );

    numFragments = ( packetSize / MaxFragmentSize ) + ( ( packetSize % MaxFragmentSize ) != 0 ? 1 : 0 );

    assert( numFragments > 0 );
    assert( numFragments <= MaxFragmentsPerPacket );

    const uint8_t *src = packetData;

    for ( int i = 0; i < numFragments; ++i )
    {
        const int fragmentSize = ( i == numFragments - 1 ) ? ( packetData + packetSize - src ) : MaxFragmentSize;

        static const int MaxFragmentPacketSize = MaxFragmentSize + PacketFragmentHeaderBytes;

        fragmentPackets[i].data = new uint8_t[MaxFragmentPacketSize];

        protocol2::WriteStream stream( fragmentPackets[i].data, MaxFragmentPacketSize );

        FragmentPacket fragmentPacket;
        fragmentPacket.fragmentSize = fragmentSize;
        fragmentPacket.crc32 = 0;
        fragmentPacket.sequence = sequence;
        fragmentPacket.fragmentId = i;
        fragmentPacket.numFragments = numFragments;
        memcpy( fragmentPacket.fragmentData, src, fragmentSize );

        if ( !fragmentPacket.SerializeWrite( stream ) )
        {
            numFragments = 0;
            for ( int j = 0; j < i; ++j )
            {
                delete fragmentPackets[i].data;
                fragmentPackets[i].data = NULL;
                fragmentPackets[i].size = 0;
            }
            return false;
        }

        stream.Flush();

        uint32_t protocolId = protocol2::host_to_network( ProtocolId );
        uint32_t crc32 = protocol2::calculate_crc32( (uint8_t*) &protocolId, 4 );
        crc32 = protocol2::calculate_crc32( fragmentPackets[i].data, stream.GetBytesProcessed(), crc32 );

        *((uint32_t*)fragmentPackets[i].data) = protocol2::host_to_network( crc32 );

        printf( "wrote fragment packet: %d bytes\n", stream.GetBytesProcessed() );

        fragmentPackets[i].size = stream.GetBytesProcessed();

        src += fragmentSize;
    }

    assert( src == packetData + packetSize );

    return true;
}

static PacketBuffer packetBuffer;

struct Vector
{
    float x,y,z;
};

inline int random_int( int min, int max )
{
    assert( max > min );
    int result = min + rand() % ( max - min + 1 );
    assert( result >= min );
    assert( result <= max );
    return result;
}

inline float random_float( float min, float max )
{
    const int res = 10000000;
    double scale = ( rand() % res ) / double( res - 1 );
    return (float) ( min + (double) ( max - min ) * scale );
}

struct TestPacketA : public protocol2::Packet
{
    int a,b,c;

    TestPacketA() : Packet( TEST_PACKET_A )
    {
        a = random_int( -10, +10 );
        b = random_int( -20, +20 );
        c = random_int( -30, +30 );
    }

    PROTOCOL2_SERIALIZE_OBJECT( stream )
    {
        serialize_int( stream, a, -10, 10 );
        serialize_int( stream, b, -20, 20 );
        serialize_int( stream, c, -30, 30 );
        return true;
    }

    bool operator == ( const TestPacketA & other ) const
    {
        return a == other.a && b == other.b && c == other.c;
    }

    bool operator != ( const TestPacketA & other ) const
    {
        return ! ( *this == other );
    }
};

static const int MaxItems = 4096;

struct TestPacketB : public protocol2::Packet
{
    int numItems;
    int items[MaxItems];

    TestPacketB() : Packet( TEST_PACKET_B )
    {
        numItems = random_int( 0, MaxItems );
        for ( int i = 0; i < numItems; ++i )
            items[i] = random_int( -100, +100 );
    }

    PROTOCOL2_SERIALIZE_OBJECT( stream )
    {
        serialize_int( stream, numItems, 0, MaxItems );
        for ( int i = 0; i < numItems; ++i )
            serialize_int( stream, items[i], -100, +100 );
        return true;
    }

    bool operator == ( const TestPacketB & other ) const
    {
        if ( numItems != other.numItems )
            return false;
        for ( int i = 0; i < numItems; ++i )
        {
            if ( items[i] != other.items[i] )
                return false;
        }
        return true;
    }

    bool operator != ( const TestPacketB & other ) const
    {
        return ! ( *this == other );
    }
};

struct TestPacketC : public protocol2::Packet
{
    Vector position;
    Vector velocity;

    TestPacketC() : Packet( TEST_PACKET_C )
    {
        position.x = random_float( -1000, +1000 );
        position.y = random_float( -1000, +1000 );
        position.z = random_float( -1000, +1000 );

        if ( rand() % 2 )
        {
            velocity.x = random_float( -100, +100 );
            velocity.y = random_float( -100, +100 );
            velocity.z = random_float( -100, +100 );
        }
        else
        {
            velocity.x = 0.0f;
            velocity.y = 0.0f;
            velocity.z = 0.0f;
        }
    }

    PROTOCOL2_SERIALIZE_OBJECT( stream )
    {
        serialize_float( stream, position.x );
        serialize_float( stream, position.y );
        serialize_float( stream, position.z );

        bool at_rest = Stream::IsWriting && velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f;

        serialize_bool( stream, at_rest );

        if ( !at_rest )
        {
            serialize_float( stream, velocity.x );
            serialize_float( stream, velocity.y );
            serialize_float( stream, velocity.z );
        }
        else
        {
            if ( Stream::IsReading )
            {
                velocity.x = 0.0f;
                velocity.y = 0.0f;
                velocity.z = 0.0f;
            }
        }

        return true;
    }

    bool operator == ( const TestPacketC & other )
    {
        return position.x == other.position.x &&
               position.y == other.position.y &&
               position.z == other.position.z &&
               velocity.x == other.velocity.x &&
               velocity.y == other.velocity.y &&
               velocity.z == other.velocity.z;
    }
};

struct TestPacketHeader : public protocol2::Object
{
    uint16_t sequence;

    PROTOCOL2_SERIALIZE_OBJECT( stream )
    {
        serialize_bits( stream, sequence, 16 );
        return true;
    }
};

struct TestPacketFactory : public protocol2::PacketFactory
{
    TestPacketFactory() : PacketFactory( TEST_PACKET_NUM_TYPES ) {}

    protocol2::Packet* CreateInternal( int type )
    {
        switch ( type )
        {
            case TEST_PACKET_A: return new TestPacketA();
            case TEST_PACKET_B: return new TestPacketB();
            case TEST_PACKET_C: return new TestPacketC();
        }
        return NULL;
    }
};

bool CheckPacketsAreIdentical( protocol2::Packet *p1, protocol2::Packet *p2 )
{
    assert( p1 );
    assert( p2 );

    if ( p1->GetType() != p2->GetType() )
        return false;

    switch ( p1->GetType() )
    {
        case TEST_PACKET_A:     return *((TestPacketA*)p1) == *((TestPacketA*)p2);
        case TEST_PACKET_B:     return *((TestPacketB*)p1) == *((TestPacketB*)p2);
        case TEST_PACKET_C:     return *((TestPacketC*)p1) == *((TestPacketC*)p2);
        default:
            return false;
    }
}

int main()
{
    srand( time( NULL ) );

    TestPacketFactory packetFactory;

    uint16_t sequence = 0;

    for ( int i = 0; i < NumIterations; ++i )
    {
        const int packetType = 1 + rand() % ( TEST_PACKET_NUM_TYPES - 1 );              // because packet type 0 indicate a packet fragment

        protocol2::Packet *writePacket = packetFactory.CreatePacket( packetType );

        assert( writePacket );
        assert( writePacket->GetType() == packetType );

        uint8_t buffer[MaxPacketSize];

        bool error = false;

        TestPacketHeader writePacketHeader;
        writePacketHeader.sequence = sequence;

        const int bytesWritten = protocol2::write_packet( writePacket, packetFactory, buffer, MaxPacketSize, ProtocolId, &writePacketHeader );

        if ( bytesWritten > 0 )
        {
            printf( "wrote packet type %d (%d bytes)\n", writePacket->GetType(), bytesWritten );
        }
        else
        {
            printf( "write packet error\n" );

            error = true;
        }

        if ( bytesWritten > MaxFragmentSize )
        {
            int numFragments;
            PacketData fragmentPackets[MaxFragmentsPerPacket];
            SplitPacketIntoFragments( sequence, buffer, bytesWritten, numFragments, fragmentPackets );

            printf( "split packet %d into %d fragments\n", sequence, numFragments );

            for ( int i = 0; i < numFragments; ++i )
            {
                printf( "process fragment packet: %d bytes\n", fragmentPackets[i].size );

                packetBuffer.ProcessPacket( fragmentPackets[i].data, fragmentPackets[i].size );
            }
        }
        else
        {
            printf( "sending packet %d as a regular packet\n", sequence );

            packetBuffer.ProcessPacket( buffer, bytesWritten );
        }

        int numPackets = 0;
        PacketData packets[PacketBufferSize];
        packetBuffer.ReceivePackets( numPackets, packets );

        for ( int i = 0; i < numPackets; ++i )
        {
            int readError;
            TestPacketHeader readPacketHeader;
            protocol2::Packet *readPacket = protocol2::read_packet( packetFactory, buffer, bytesWritten, ProtocolId, &readPacketHeader, &readError );

            if ( readPacket )
            {
                printf( "read packet type %d (%d bytes)\n", readPacket->GetType(), bytesWritten );

                if ( !CheckPacketsAreIdentical( readPacket, writePacket ) )
                {
                    printf( "read packet is not the same as written packet. something wrong with serialize function?\n" );
                    error = true;
                }
            }
            else
            {
                printf( "read packet error: %s\n", protocol2::error_string( readError ) );

                error = true;
            }

            packetFactory.DestroyPacket( readPacket );

            if ( error )
                break;
        }

        packetFactory.DestroyPacket( writePacket );

        if ( error )
            return 1;

        sequence++;
    }

    return 0;
}
