#include "MsQuicWriterNode.h"

MsQuicWriterNode::MsQuicWriterNode(short msgId, int64_t dataSize, short headSize, std::vector<char> data):msgId(msgId),dataSize(dataSize), headSize(headSize),data(data)
{
}

MsQuicWriterNode::MsQuicWriterNode()
{
}

MsQuicWriterNode::~MsQuicWriterNode()
{
}
