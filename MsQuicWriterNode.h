#pragma once
#include <iostream>
#include <vector>

class MsQuicWriterNode
{
public:

	MsQuicWriterNode(short msgId, int64_t dataSize, short headSize, std::vector<char> data);

	MsQuicWriterNode();

	~MsQuicWriterNode();

	short msgId;

	int64_t dataSize;

	short headSize;

	std::vector<char> data;
};

