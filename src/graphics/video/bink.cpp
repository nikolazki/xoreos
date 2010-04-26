/* eos - A reimplementation of BioWare's Aurora engine
 * Copyright (c) 2010 Sven Hesse (DrMcCoy), Matthew Hoops (clone2727)
 *
 * The Infinity, Aurora, Odyssey and Eclipse engines, Copyright (c) BioWare corp.
 * The Electron engine, Copyright (c) Obsidian Entertainment and BioWare corp.
 *
 * This file is part of eos and is distributed under the terms of
 * the GNU General Public Licence. See COPYING for more informations.
 */

/** @file graphics/video/bink.cpp
 *  Decoding RAD Game Tools' Bink videos.
 */

#include <cmath>

#include "common/util.h"
#include "common/error.h"
#include "common/stream.h"
#include "common/bitstream.h"
#include "common/huffman.h"

#include "graphics/video/bink.h"
#include "graphics/video/binkdata.h"

#include "events/events.h"

static const uint32 kBIKfID = MKID_BE('BIKf');
static const uint32 kBIKgID = MKID_BE('BIKg');
static const uint32 kBIKhID = MKID_BE('BIKh');
static const uint32 kBIKiID = MKID_BE('BIKi');

static const uint32 kVideoFlagAlpha = 0x00100000;

// Number of bits used to store first DC value in bundle
static const uint32 kDCStartBits = 11;

namespace Graphics {

Bink::Bink(Common::SeekableReadStream *bink) : _bink(bink), _curFrame(0) {
	assert(_bink);

	for (int i = 0; i < 16; i++)
		_huffman[i] = 0;

	for (int i = 0; i < kSourceMAX; i++) {
		_bundles[i].countLength = 0;

		_bundles[i].huffman.index = 0;
		for (int j = 0; j < 16; j++)
			_bundles[i].huffman.symbols[j] = j;

		_bundles[i].data     = 0;
		_bundles[i].dataEnd  = 0;
		_bundles[i].curDec   = 0;
		_bundles[i].curPtr   = 0;
	}

	for (int i = 0; i < 16; i++) {
		_colHighHuffman[i].index = 0;
		for (int j = 0; j < 16; j++)
			_colHighHuffman[i].symbols[j] = j;
	}

	for (int i = 0; i < 4; i++)
		_planes[i] = 0;

	load();
	addToQueue();
}

Bink::~Bink() {
	for (int i = 0; i < 4; i++)
		delete[] _planes[i];

	deinitBundles();

	for (int i = 0; i < 16; i++)
		delete _huffman[i];

	delete _bink;
}

bool Bink::gotTime() const {
	uint32 curTime = EventMan.getTimestamp();
	uint32 frameTime = ((uint64) (_curFrame * 1000 * ((uint64) _fpsDen))) / _fpsNum;
	if ((curTime - _startTime + 11) < frameTime)
		return true;

	return false;
}

void Bink::processData() {
	uint32 curTime = EventMan.getTimestamp();

	if (!_started) {
		_startTime     = curTime;
		_lastFrameTime = curTime;
		_started       = true;
	}

	uint32 frameTime = ((uint64) (_curFrame * 1000 * ((uint64) _fpsDen))) / _fpsNum;
	if ((curTime - _startTime) < frameTime)
		return;

	if (_curFrame >= _frames.size()) {
		_finished = true;
		return;
	}

	VideoFrame &frame = _frames[_curFrame];

	if (!_bink->seek(frame.offset))
		throw Common::Exception(Common::kSeekError);

	uint32 frameSize = frame.size;

	for (std::vector<AudioTrack>::iterator audio = _audioTracks.begin(); audio != _audioTracks.end(); ++audio) {
		uint32 audioPacketLength = _bink->readUint32LE();

		if (frameSize < (audioPacketLength + 4))
			throw Common::Exception("Audio packet too big for the frame");

		if (audioPacketLength >= 4) {
			audio->sampleCount = _bink->readUint32LE();

			audio->bits = new Common::BitStream32LE(*_bink, (audioPacketLength - 4) * 8);

			audioPacket(*audio);

			delete audio->bits;
			audio->bits = 0;

			frameSize -= audioPacketLength + 4;
		}
	}

	frame.bits = new Common::BitStream32LE(*_bink, frameSize * 8);

	videoPacket(frame);

	delete frame.bits;
	frame.bits = 0;

	_needCopy = true;

	warning("Frame %d / %d", _curFrame, (int) _frames.size());

	_curFrame++;
}

void Bink::yuva2bgra() {
	assert(_planes[0] && _planes[1] && _planes[2] && _planes[3]);

	// TODO: :P

	const byte *planeY = _planes[0];
	const byte *planeU = _planes[1];
	const byte *planeV = _planes[2];
	const byte *planeA = _planes[3];
	byte *data = _data + (_height - 1) * _pitch * 4;
	for (uint32 y = 0; y < _height; y++) {
		byte *rowData = data;

		for (uint32 x = 0; x < _width; x++, rowData += 4) {
			rowData[0] = planeY[x];
			rowData[1] = planeY[x];
			rowData[2] = planeY[x];
			rowData[3] = planeA[x];
		}

		data   -= _pitch * 4;
		planeY += _width;
		planeU += _width >> 1;
		planeV += _width >> 1;
		planeA += _width;
	}


/*
	byte tmpg[4] = {   0, 255,   0, 255 };
	byte tmpr[4] = {   0,   0, 255, 255 };
	byte tmpb[4] = { 255,   0,   0, 255 };
	byte tmps[4] = { 255,   0, 255, 255 };
	data = _data;

	for (uint32 i = 0; i < _width; i++, data += 4)
		memcpy(data, tmpg, 4);

	data = _data + (_height - 1 ) * _pitch * 4;
	for (uint32 i = 0; i < _width; i++, data += 4)
		memcpy(data, tmpr, 4);

	data = _data;
	for (uint32 i = 0; i < _height; i++, data += _pitch * 4)
		memcpy(data, tmpb, 4);

	data = _data + (_width - 1) * 4;
	for (uint32 i = 0; i < _height; i++, data += _pitch * 4)
		memcpy(data, tmps, 4);
*/
}

void Bink::audioPacket(AudioTrack &audio) {
}

void Bink::videoPacket(VideoFrame &video) {
	assert(video.bits);

	if (_hasAlpha) {
		if (_id == kBIKiID)
			video.bits->skip(32);

		decodePlane(video, 3, false);
	}

	if (_id == kBIKiID)
		video.bits->skip(32);

	for (int i = 0; i < 3; i++) {
		int planeIdx = ((i == 0) || !_swapPlanes) ? i : (i ^ 3);

		decodePlane(video, planeIdx, i != 0);

		if (video.bits->pos() >= video.bits->size())
			break;
	}

	// And convert the YUVA data we have to BGRA
	yuva2bgra();
}

void Bink::decodePlane(VideoFrame &video, int planeIdx, bool isChroma) {

	uint32 bw    = isChroma ? ((_width  + 15) >> 4) : ((_width  + 7) >> 3);
	uint32 bh    = isChroma ? ((_height + 15) >> 4) : ((_height + 7) >> 3);
	uint32 width = isChroma ?  (_width        >> 1) :   _width;

	// const int stride = c->pic.linesize[plane_idx];

	// ref_start = c->last.data[plane_idx];
	// ref_end   = c->last.data[plane_idx] + (bw - 1 + c->last.linesize[plane_idx] * (bh - 1)) * 8;

	// int coordmap[64];
	// for (int i = 0; i < 64; i++)
		// coordmap[i] = (i & 7) + (i >> 3) * stride;

	// const uint8_t *scan;
	// DECLARE_ALIGNED(16, DCTELEM, block[64]);
	// DECLARE_ALIGNED(16, uint8_t, ublock[64]);

	initLengths(MAX<uint32>(width, 8), bw);
	for (int i = 0; i < kSourceMAX; i++)
		readBundle(video, (Source) i);

	for (uint32 by = 0; by < bh; by++) {
		readBlockTypes  (video, _bundles[kSourceBlockTypes]);
		readBlockTypes  (video, _bundles[kSourceSubBlockTypes]);
		readColors      (video, _bundles[kSourceColors]);
		readPatterns    (video, _bundles[kSourcePattern]);
		readMotionValues(video, _bundles[kSourceXOff]);
		readMotionValues(video, _bundles[kSourceYOff]);
		readDCS         (video, _bundles[kSourceIntraDC], kDCStartBits, false);
		readDCS         (video, _bundles[kSourceInterDC], kDCStartBits, true);
		readRuns        (video, _bundles[kSourceRun]);

		byte *dst = _planes[planeIdx] + 8 * by * width;

		// prev = c->last.data[plane_idx] + 8*by*stride;

		for (uint32 bx = 0; bx < bw; bx++, dst += 8) { // prev += 8;
			BlockType blockType = (BlockType) getBundleValue(kSourceBlockTypes);

			// warning("%d.%d.%d: %d (%d)", planeIdx, by, bx, blockType, video.bits->pos());

			// 16x16 block type on odd line means part of the already decoded block, so skip it
			if ((by & 1) && (blockType == kBlockScaled)) {
				bx++;
				dst += 8;
				// prev += 8;
				continue;
			}

			switch (blockType) {
				case kBlockSkip:
					blockSkip(video);
					break;

				case kBlockScaled:
					blockScaled(video, bx, dst, width);
					break;

				case kBlockMotion:
					blockMotion(video);
					break;

				case kBlockRun:
					blockRun(video);
					break;

				case kBlockResidue:
					blockResidue(video);
					break;

				case kBlockIntra:
					blockIntra(video);
					break;

				case kBlockFill:
					blockFill(video, dst, width);
					break;

				case kBlockInter:
					blockInter(video);
					break;

				case kBlockPattern:
					blockPattern(video, dst, width);
					break;

				case kBlockRaw:
					blockRaw(video, dst, width);
					break;

				default:
					throw Common::Exception("Unknown block type: %d", blockType);
			}

		}

	}

	if (video.bits->pos() & 0x1F) // next plane data starts at 32-bit boundary
		video.bits->skip(32 - (video.bits->pos() & 0x1F));

}

void Bink::readBundle(VideoFrame &video, Source source) {
	if (source == kSourceColors) {
		for (int i = 0; i < 16; i++)
			readHuffman(video, _colHighHuffman[i]);

		_colLastVal = 0;
	}

	if ((source != kSourceIntraDC) && (source != kSourceInterDC))
		readHuffman(video, _bundles[source].huffman);

	_bundles[source].curDec = _bundles[source].data;
	_bundles[source].curPtr = _bundles[source].data;
}

void Bink::readHuffman(VideoFrame &video, Huffman &huffman) {
	huffman.index = video.bits->getBits(4);

	if (huffman.index == 0) {
		// The first tree always gives raw nibbles

		for (int i = 0; i < 16; i++)
			huffman.symbols[i] = i;

		return;
	}

	byte hasSymbol[16];

	if (video.bits->getBit()) {
		// Symbol selection

		memset(hasSymbol, 0, 16);

		uint8 length = video.bits->getBits(3);
		for (int i = 0; i <= length; i++) {
			huffman.symbols[i] = video.bits->getBits(4);
			hasSymbol[huffman.symbols[i]] = 1;
		}

		for (int i = 0; i < 16; i++)
			if (hasSymbol[i] == 0)
				huffman.symbols[++length] = i;

		return;
	}

	// Symbol shuffling

	byte tmp1[16], tmp2[16];
	byte *in = tmp1, *out = tmp2;

	uint8 depth = video.bits->getBits(2);

	for (int i = 0; i < 16; i++)
		in[i] = i;

	for (int i = 0; i <= depth; i++) {
		int size = 1 << i;

		for (int j = 0; j < 16; j += (size << 1))
			mergeHuffmanSymbols(video, out + j, in + j, size);

		SWAP(in, out);
	}

	memcpy(huffman.symbols, in, 16);
}

void Bink::mergeHuffmanSymbols(VideoFrame &video, byte *dst, byte *src, int size) {
	byte *src2  = src + size;
	int   size2 = size;

	do {
		if (!video.bits->getBit()) {
			*dst++ = *src++;
			size--;
		} else {
			*dst++ = *src2++;
			size2--;
		}

	} while (size && size2);

	while (size--)
		*dst++ = *src++;
	while (size2--)
		*dst++ = *src2++;
}

void Bink::load() {
	_id = _bink->readUint32BE();
	if ((_id != kBIKfID) && (_id != kBIKgID) && (_id != kBIKhID) && (_id != kBIKiID))
		throw Common::Exception("Unknown Bink FourCC %04X", _id);

	uint32 fileSize         = _bink->readUint32LE() + 8;
	uint32 frameCount       = _bink->readUint32LE();
	uint32 largestFrameSize = _bink->readUint32LE();

	if (largestFrameSize > fileSize)
		throw Common::Exception("Largest frame size greater than file size");

	_bink->skip(4);

	uint32 width  = _bink->readUint32LE();
	uint32 height = _bink->readUint32LE();

	createData(width, height);

	_fpsNum = _bink->readUint32LE();
	_fpsDen = _bink->readUint32LE();

	if ((_fpsNum == 0) || (_fpsDen == 0))
		throw Common::Exception("Invalid FPS (%d/%d)", _fpsNum, _fpsDen);

	_videoFlags = _bink->readUint32LE();

	uint32 audioTrackCount = _bink->readUint32LE();
	if (audioTrackCount > 0) {
		_audioTracks.resize(audioTrackCount);

		_bink->skip(4 * audioTrackCount);

		// Reading audio track properties
		for (std::vector<AudioTrack>::iterator it = _audioTracks.begin(); it != _audioTracks.end(); ++it) {
			it->sampleRate  = _bink->readUint16LE();
			it->flags       = _bink->readUint16LE();
			it->sampleCount = 0;
			it->bits        = 0;
		}

		_bink->skip(4 * audioTrackCount);
	}

	// Reading video frame properties
	_frames.resize(frameCount);
	for (uint32 i = 0; i < frameCount; i++) {
		_frames[i].offset   = _bink->readUint32LE();
		_frames[i].keyFrame = _frames[i].offset & 1;

		_frames[i].offset &= ~1;

		if (i != 0)
			_frames[i - 1].size = _frames[i].offset - _frames[i - 1].offset;

		_frames[i].bits = 0;
	}

	_frames[frameCount - 1].size = _bink->size() - _frames[frameCount - 1].offset;

	_hasAlpha   = _videoFlags & kVideoFlagAlpha;
	_swapPlanes = (_id == kBIKhID) || (_id == kBIKiID); // BIKh and BIKi swap the chroma planes

	_planes[0] = new byte[ _width       *  _height      ]; // Y
	_planes[1] = new byte[(_width >> 1) * (_height >> 1)]; // U, 1/4 resolution
	_planes[2] = new byte[(_width >> 1) * (_height >> 1)]; // V, 1/4 resolution
	_planes[3] = new byte[ _width       *  _height      ]; // A

	// Initialize the video with solid black
	memset(_planes[0],   0,  _width       *  _height      );
	memset(_planes[1],   0, (_width >> 1) * (_height >> 1));
	memset(_planes[2],   0, (_width >> 1) * (_height >> 1));
	memset(_planes[3], 255,  _width       *  _height      );

	initBundles();
	initHuffman();
}

void Bink::initBundles() {
	uint32 bw     = (_width  + 7) >> 3;
	uint32 bh     = (_height + 7) >> 3;
	uint32 blocks = bw * bh;

	for (int i = 0; i < kSourceMAX; i++) {
		_bundles[i].data    = new byte[blocks * 64];
		_bundles[i].dataEnd = _bundles[i].data + blocks * 64;
	}
}

void Bink::deinitBundles() {
	for (int i = 0; i < kSourceMAX; i++)
		delete[] _bundles[i].data;
}

void Bink::initHuffman() {
	for (int i = 0; i < 16; i++)
		_huffman[i] = new Common::Huffman(binkHuffmanLengths[i][15], 16, binkHuffmanCodes[i], binkHuffmanLengths[i]);
}

// TODO: Don't use the slow standard log2()
void Bink::initLengths(uint32 width, uint32 bw) {
	_bundles[kSourceBlockTypes   ].countLength = log2((width >> 3)    + 511) + 1;
	_bundles[kSourceSubBlockTypes].countLength = log2((width >> 4)    + 511) + 1;
	_bundles[kSourceColors       ].countLength = log2((width >> 3)*64 + 511) + 1;
	_bundles[kSourceIntraDC      ].countLength = log2((width >> 3)    + 511) + 1;
	_bundles[kSourceInterDC      ].countLength = log2((width >> 3)    + 511) + 1;
	_bundles[kSourceXOff         ].countLength = log2((width >> 3)    + 511) + 1;
	_bundles[kSourceYOff         ].countLength = log2((width >> 3)    + 511) + 1;
	_bundles[kSourcePattern      ].countLength = log2((bw    << 3)    + 511) + 1;
	_bundles[kSourceRun          ].countLength = log2((width >> 3)*48 + 511) + 1;
}

byte Bink::getHuffmanSymbol(VideoFrame &video, Huffman &huffman) {
	return huffman.symbols[_huffman[huffman.index]->getSymbol(*video.bits)];
}

int32 Bink::getBundleValue(Source source) {
	if ((source < kSourceXOff) || (source == kSourceRun))
		return *_bundles[source].curPtr++;

	if ((source == kSourceXOff) || (source == kSourceYOff))
		return (int8) *_bundles[source].curPtr++;

	int16 ret = *((int16 *) _bundles[source].curPtr);

	_bundles[source].curPtr += 2;

	return ret;
}

uint32 Bink::readBundleCount(VideoFrame &video, Bundle &bundle) {
	if (!bundle.curDec || (bundle.curDec > bundle.curPtr))
		return 0;

	uint32 n = video.bits->getBits(bundle.countLength);
	if (n == 0)
		bundle.curDec = 0;

	return n;
}

void Bink::blockSkip(VideoFrame &video) {
	// c->dsp.put_pixels_tab[1][0](dst, prev, stride, 8);
}

void Bink::blockScaledRun(VideoFrame &video) {
	video.bits->getBits(4); //scan = bink_patterns[get_bits(gb, 4)];

	int i = 0;
	do {
		int run = getBundleValue(kSourceRun) + 1;

		i += run;
		if (i > 64)
			throw Common::Exception("Run went out of bounds");

		if (video.bits->getBit()) {

			byte v = getBundleValue(kSourceColors);
			for (int j = 0; j < run; j++)
				; // ublock[*scan++] = v;

		} else
			for (int j = 0; j < run; j++)
				getBundleValue(kSourceColors); // ublock[*scan++] = get_value(c, BINK_SRC_COLORS);

	} while (i < 63);

	if (i == 63)
		getBundleValue(kSourceColors); // ublock[*scan++] = get_value(c, BINK_SRC_COLORS);
}

void Bink::blockScaledIntra(VideoFrame &video) {
	// c->dsp.clear_block(block);
	getBundleValue(kSourceIntraDC); //block[0] = get_value(c, BINK_SRC_INTRA_DC);
	readDCTCoeffs(video, 0, 0, 1); // read_dct_coeffs(gb, block, c->scantable.permutated, 1);
	// c->dsp.idct(block);
	// c->dsp.put_pixels_nonclamped(block, ublock, 8);
}

void Bink::blockScaledFill(VideoFrame &video, byte *dst, uint32 pitch) {
	byte v = getBundleValue(kSourceColors);

	for (int i = 0; i < 16; i++, dst += pitch)
		memset(dst, v, 16);
}

void Bink::blockScaledPattern(VideoFrame &video, byte *dst, uint32 pitch) {
	byte col[2];

	for (int i = 0; i < 2; i++)
		col[i] = getBundleValue(kSourceColors);

	byte *dst2 = dst + pitch;
	for (int j = 0; j < 8; j++, dst += (pitch << 1) - 16, dst2 += (pitch << 1) - 16) {
		byte v = getBundleValue(kSourcePattern);

		for (int i = 0; i < 8; i++, v >>= 1)
			dst[0] = dst[1] = dst2[0] = dst2[1] = col[v & 1];
	}
}

void Bink::blockScaledRaw(VideoFrame &video, byte *dst, uint32 pitch) {
	byte row[8];

	byte *dst2 = dst + pitch;
	for (int j = 0; j < 8; j++, dst += (pitch << 1) - 16, dst2 += (pitch << 1) - 16) {
		memcpy(row, _bundles[kSourceColors].curPtr, 8);

		for (int i = 0; i < 8; i++, dst += 2)
			dst[0] = dst[1] = dst2[0] = dst2[1] = row[i];

		_bundles[kSourceColors].curPtr += 8;
	}
}

void Bink::blockScaled(VideoFrame &video, uint32 &bx, byte *&dst, uint32 pitch) {
	BlockType blockType = (BlockType) getBundleValue(kSourceSubBlockTypes);
	// warning("blockScaled: %d", blockType);

	switch (blockType) {
		case kBlockRun:
			blockScaledRun(video);
			break;

		case kBlockIntra:
			blockScaledIntra(video);
			break;

		case kBlockFill:
			blockScaledFill(video, dst, pitch);
			break;

		case kBlockPattern:
			blockScaledPattern(video, dst, pitch);
			break;

		case kBlockRaw:
			blockScaledRaw(video, dst, pitch);
			break;

		default:
			throw Common::Exception("Invalid 16x16 block type: %d\n", blockType);
	}

	bx++;
	dst += 8;
	// prev += 8;
}

void Bink::blockMotion(VideoFrame &video) {
	int8 xOff = getBundleValue(kSourceXOff);
	int8 yOff = getBundleValue(kSourceYOff);

	// ref = prev + xoff + yoff * stride;

	// if (ref < ref_start || ref > ref_end)
		// throw Common::Exception("Copy out of bounds (%d | %d)", bx * 8 + xOff, by * 8 + yOff);

	// c->dsp.put_pixels_tab[1][0](dst, ref, stride, 8);
}

void Bink::blockRun(VideoFrame &video) {
	video.bits->getBits(4); // scan = bink_patterns[get_bits(gb, 4)];

	int i = 0;
	do {
		int run = getBundleValue(kSourceRun) + 1;

		i += run;
		if (i > 64)
			throw Common::Exception("Run went out of bounds");

		if (video.bits->getBit()) {

			byte v = getBundleValue(kSourceColors);
			for (int j = 0; j < run; j++)
				; // dst[coordmap[*scan++]] = v;

		} else
			for (int j = 0; j < run; j++)
				getBundleValue(kSourceColors); // dst[coordmap[*scan++]] = get_value(c, BINK_SRC_COLORS);

	} while (i < 63);

	if (i == 63)
		getBundleValue(kSourceColors); // dst[coordmap[*scan++]] = get_value(c, BINK_SRC_COLORS);
}

void Bink::blockResidue(VideoFrame &video) {
	int8 xOff = getBundleValue(kSourceXOff);
	int8 yOff = getBundleValue(kSourceYOff);

	// ref = prev + xoff + yoff * stride;

	// if (ref < ref_start || ref > ref_end)
		// throw Common::Exception("Copy out of bounds (%d | %d)", bx * 8 + xOff, by * 8 + yOff);

	// c->dsp.put_pixels_tab[1][0](dst, ref, stride, 8);
	// c->dsp.clear_block(block);

	byte v = video.bits->getBits(7);

	readResidue(video, 0, v); // read_residue(gb, block, v);
	// c->dsp.add_pixels8(dst, block, stride);
}

void Bink::blockIntra(VideoFrame &video) {
	// c->dsp.clear_block(block);

	getBundleValue(kSourceIntraDC); // block[0] = get_value(c, BINK_SRC_INTRA_DC);

	readDCTCoeffs(video, 0, 0, 1); // read_dct_coeffs(gb, block, c->scantable.permutated, 1);

	// c->dsp.idct_put(dst, stride, block);
}

void Bink::blockFill(VideoFrame &video, byte *dst, uint32 pitch) {
	byte v = getBundleValue(kSourceColors);

	for (int i = 0; i < 8; i++, dst += pitch)
		memset(dst, v, 8);
}

void Bink::blockInter(VideoFrame &video) {
	int8 xOff = getBundleValue(kSourceXOff);
	int8 yOff = getBundleValue(kSourceYOff);

	// ref = prev + xoff + yoff * stride;
	// c->dsp.put_pixels_tab[1][0](dst, ref, stride, 8);
	// c->dsp.clear_block(block);

	getBundleValue(kSourceInterDC); // block[0] = get_value(c, BINK_SRC_INTER_DC);

	readDCTCoeffs(video, 0, 0, 0); // read_dct_coeffs(gb, block, c->scantable.permutated, 0);

	// c->dsp.idct_add(dst, stride, block);
}

void Bink::blockPattern(VideoFrame &video, byte *dst, uint32 pitch) {
	byte col[2];

	for (int i = 0; i < 2; i++)
		col[i] = getBundleValue(kSourceColors);


	for (int i = 0; i < 8; i++) {
		byte v = getBundleValue(kSourcePattern);

		for (int j = 0; j < 8; j++, v >>= 1)
			dst[i * pitch + j] = col[v & 1];
	}
}

void Bink::blockRaw(VideoFrame &video, byte *dst, uint32 pitch) {
	for (int i = 0; i < 8; i++)
		memcpy(dst + i * pitch, _bundles[kSourceColors].curPtr + i * 8, 8);

	_bundles[kSourceColors].curPtr += 64;
}

void Bink::readRuns(VideoFrame &video, Bundle &bundle) {
	uint32 n = readBundleCount(video, bundle);
	if (n == 0)
		return;

	byte *decEnd = bundle.curDec + n;
	if (decEnd > bundle.dataEnd)
		throw Common::Exception("Run value went out of bounds");

	if (video.bits->getBit()) {
		byte v = video.bits->getBits(4);

		memset(bundle.curDec, v, n);
		bundle.curDec += n;

	} else
		while (bundle.curDec < decEnd)
			*bundle.curDec++ = getHuffmanSymbol(video, bundle.huffman);
}

void Bink::readMotionValues(VideoFrame &video, Bundle &bundle) {
	uint32 n = readBundleCount(video, bundle);
	if (n == 0)
		return;

	byte *decEnd = bundle.curDec + n;
	if (decEnd > bundle.dataEnd)
		throw Common::Exception("Too many motion values");

	if (video.bits->getBit()) {
		byte v = video.bits->getBits(4);

		if (v) {
			int sign = -video.bits->getBit();
			v = (v ^ sign) - sign;
		}

		memset(bundle.curDec, v, n);

		bundle.curDec += n;
		return;
	}

	do {
		byte v = getHuffmanSymbol(video, bundle.huffman);

		if (v) {
			int sign = -video.bits->getBit();
			v = (v ^ sign) - sign;
		}

		*bundle.curDec++ = v;

	} while (bundle.curDec < decEnd);
}

const uint8 rleLens[4] = { 4, 8, 12, 32 };
void Bink::readBlockTypes(VideoFrame &video, Bundle &bundle) {
	uint32 n = readBundleCount(video, bundle);
	if (n == 0)
		return;

	byte *decEnd = bundle.curDec + n;
	if (decEnd > bundle.dataEnd)
		throw Common::Exception("Too many block type values");

	if (video.bits->getBit()) {
		byte v = video.bits->getBits(4);

		memset(bundle.curDec, v, n);

		bundle.curDec += n;
		return;
	}

	byte last = 0;
	do {

		byte v = getHuffmanSymbol(video, bundle.huffman);

		if (v < 12) {
			last = v;
			*bundle.curDec++ = v;
		} else {
			int run = rleLens[v - 12];

			memset(bundle.curDec, last, run);

			bundle.curDec += run;
		}

	} while (bundle.curDec < decEnd);
}

void Bink::readPatterns(VideoFrame &video, Bundle &bundle) {
	uint32 n = readBundleCount(video, bundle);
	if (n == 0)
		return;

	byte *decEnd = bundle.curDec + n;
	if (decEnd > bundle.dataEnd)
		throw Common::Exception("Too many pattern values");

	byte v;
	while (bundle.curDec < decEnd) {
		v  = getHuffmanSymbol(video, bundle.huffman);
		v |= getHuffmanSymbol(video, bundle.huffman) << 4;
		*bundle.curDec++ = v;
	}
}


void Bink::readColors(VideoFrame &video, Bundle &bundle) {
	uint32 n = readBundleCount(video, bundle);
	if (n == 0)
		return;

	byte *decEnd = bundle.curDec + n;
	if (decEnd > bundle.dataEnd)
		throw Common::Exception("Too many color values");

	if (video.bits->getBit()) {
		_colLastVal = getHuffmanSymbol(video, _colHighHuffman[_colLastVal]);

		byte v;
		v = getHuffmanSymbol(video, bundle.huffman);
		v = (_colLastVal << 4) | v;

		if (_id != kBIKiID) {
			int sign = ((int8) v) >> 7;
			v = ((v & 0x7F) ^ sign) - sign;
			v += 0x80;
		}

		memset(bundle.curDec, v, n);
		bundle.curDec += n;

		return;
	}

	while (bundle.curDec < decEnd) {
		_colLastVal = getHuffmanSymbol(video, _colHighHuffman[_colLastVal]);

		byte v;
		v = getHuffmanSymbol(video, bundle.huffman);
		v = (_colLastVal << 4) | v;

		if (_id != kBIKiID) {
			int sign = ((int8) v) >> 7;
			v = ((v & 0x7F) ^ sign) - sign;
			v += 0x80;
		}
		*bundle.curDec++ = v;
	}
}

void Bink::readDCS(VideoFrame &video, Bundle &bundle, int startBits, bool hasSign) {
	uint32 length = readBundleCount(video, bundle);
	if (length == 0)
		return;

	int16 *dst = (int16 *) bundle.curDec;

	int32 v = video.bits->getBits(startBits - (hasSign ? 1 : 0));
	if (v && hasSign) {
		int sign = -video.bits->getBit();
		v = (v ^ sign) - sign;
	}

	*dst++ = v;
	length--;

	for (uint32 i = 0; i < length; i += 8) {
		uint32 length2 = MIN<uint32>(length - i, 8);

		byte bSize = video.bits->getBits(4);

		if (bSize) {

			for (uint32 j = 0; j < length2; j++) {
				int16 v2 = video.bits->getBits(bSize);
				if (v2) {
					int sign = -video.bits->getBit();
					v2 = (v2 ^ sign) - sign;
				}

				v += v2;
				*dst++ = v;

				if ((v < -32768) || (v > 32767))
					throw Common::Exception("DC value went out of bounds: %d", v);
			}

		} else
			for (uint32 j = 0; j < length2; j++)
				*dst++ = v;
	}

	bundle.curDec = (byte *) dst;
}

/** Reads 8x8 block of DCT coefficients. */
void Bink::readDCTCoeffs(VideoFrame &video, void *block, void *scan, int isIntra) {
	throw Common::Exception("TODO: readDCTCoeffs");
}

/** Reads 8x8 block with residue after motion compensation. */
void Bink::readResidue(VideoFrame &video, void *block, int masksCount) {
	throw Common::Exception("TODO: readResidue");
}

} // End of namespace Graphics
