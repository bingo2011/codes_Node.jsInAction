#include "WavReader.h"
#include "WavDescriptor.h"
#include "Snippet.h"

#include <iostream>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include <rlog/rlog.h>
#include <rlog/StdioNode.h>

using namespace std;
using namespace boost::filesystem;
using namespace rlog;

bool hasExtension(const string& filename, const string& s) {
   string ext{"." + s};
   if (ext.length() > filename.length()) return false;
   return 0 == 
      filename.compare(filename.length() - ext.length(), ext.length(), ext);
}

struct RiffHeader {
   int8_t id[4];
   uint32_t length;
   int8_t format[4];
};

struct FormatSubchunkHeader {
   int8_t id[4];
   uint32_t subchunkSize;
};

struct FactOrData {
   int8_t tag[4];
};

struct FactChunk {
   uint32_t chunkSize;
   uint32_t samplesPerChannel;
};

WavReader::WavReader(
      const std::string& source, 
      const std::string& dest,
      shared_ptr<WavDescriptor> descriptor) 
   : source_(source)
   , dest_(dest)
   , descriptor_(descriptor) {
   if (!descriptor_)
      descriptor_ = make_shared<WavDescriptor>(dest);

   channel = DEF_CHANNEL("info/wav", Log_Debug);
   log.subscribeTo((RLogNode*)RLOG_CHANNEL("info/wav"));
   rLog(channel, "reading from %s writing to %s", source.c_str(), dest.c_str());
}

WavReader::~WavReader() {
   descriptor_.reset();
   delete channel;
}

void WavReader::useFileUtil(shared_ptr<FileUtil> fileUtil) {
   fileUtil_ = fileUtil;
}

void WavReader::publishSnippets() {
   directory_iterator itEnd; 
   for (directory_iterator it(source_); it != itEnd; ++it) 
      if (!is_directory(it->status()) && 
//  it->path().filename().string()=="husten.wav" &&
          hasExtension(it->path().filename().string(), "wav"))
        open(it->path().filename().string(), false);
}

string WavReader::toString(int8_t* bytes, unsigned int size) {
   return string{(char*)bytes, size};
}

void WavReader::open(const std::string& name, bool trace) {
   // ...
   rLog(channel, "opening %s", name.c_str());

   ifstream file{name, ios::in | ios::binary};
   if (!file.is_open()) {
      rLog(channel, "unable to read %s", name.c_str());
      return;
   }

   ofstream out{dest_ + "/" + name, ios::out | ios::binary};

   FormatSubchunk formatSubchunk;
   FormatSubchunkHeader formatSubchunkHeader;
   readAndWriteHeaders(name, file, out, formatSubchunk, formatSubchunkHeader);

   DataChunk dataChunk;
   read(file, dataChunk);

   rLog(channel, "riff header size = %i" , sizeof(RiffHeader));
   rLog(channel, "subchunk header size = %i", sizeof(FormatSubchunkHeader));
   rLog(channel, "subchunk size = %i", formatSubchunkHeader.subchunkSize);
   rLog(channel, "data length = %i", dataChunk.length);
   
   auto data = readData(file, dataChunk.length); // leak!
   
   writeSnippet(name, file, out, formatSubchunk, dataChunk, data);
}

void WavReader::read(istream& file, DataChunk& dataChunk) {
   file.read(reinterpret_cast<char*>(&dataChunk), sizeof(DataChunk));
}

char* WavReader::readData(istream& file, int32_t length) {
   auto data = new char[length];
   file.read(data, length);
   //file.close(); // istreams are RAII
   return data;
}

void WavReader::readAndWriteHeaders(
      const std::string& name,
      istream& file,
      ostream& out,
      FormatSubchunk& formatSubchunk,
      FormatSubchunkHeader& formatSubchunkHeader) {
   RiffHeader header;
   file.read(reinterpret_cast<char*>(&header), sizeof(RiffHeader));
   // ...

   if (toString(header.id, 4) != "RIFF") {
      rLog(channel, "ERROR: %s is not a RIFF file",
         name.c_str());
      return;
   }
   if (toString(header.format, 4) != "WAVE") {
      rLog(channel, "ERROR: %s is not a wav file: %s",
         name.c_str(),
         toString(header.format, 4).c_str());
      return;
   }
   out.write(reinterpret_cast<char*>(&header), sizeof(RiffHeader));

   file.read(reinterpret_cast<char*>(&formatSubchunkHeader), 
         sizeof(FormatSubchunkHeader));

   if (toString(formatSubchunkHeader.id, 4) != "fmt ") {
      rLog(channel, "ERROR: %s expecting 'fmt' for subchunk header; got '%s'",
         name.c_str(),
         toString(formatSubchunkHeader.id, 4).c_str());
      return;
   }

   out.write(reinterpret_cast<char*>(&formatSubchunkHeader), 
         sizeof(FormatSubchunkHeader));

   file.read(reinterpret_cast<char*>(&formatSubchunk), sizeof(FormatSubchunk));

   out.write(reinterpret_cast<char*>(&formatSubchunk), sizeof(FormatSubchunk));

   rLog(channel, "format tag: %u", formatSubchunk.formatTag); // show as hex?
   rLog(channel, "samples per second: %u", formatSubchunk.samplesPerSecond);
   rLog(channel, "channels: %u", formatSubchunk.channels);
   rLog(channel, "bits per sample: %u", formatSubchunk.bitsPerSample);

   auto bytes = formatSubchunkHeader.subchunkSize - sizeof(FormatSubchunk);

   auto additionalBytes = new char[bytes];
   file.read(additionalBytes, bytes);
   out.write(additionalBytes, bytes);
   
   FactOrData factOrData;
   file.read(reinterpret_cast<char*>(&factOrData), sizeof(FactOrData));
   out.write(reinterpret_cast<char*>(&factOrData), sizeof(FactOrData));

   if (toString(factOrData.tag, 4) == "fact") {
      FactChunk factChunk;
      file.read(reinterpret_cast<char*>(&factChunk), sizeof(FactChunk));
      out.write(reinterpret_cast<char*>(&factChunk), sizeof(FactChunk));

      file.read(reinterpret_cast<char*>(&factOrData), sizeof(FactOrData));
      out.write(reinterpret_cast<char*>(&factOrData), sizeof(FactOrData));

      rLog(channel, "samples per channel: %u", factChunk.samplesPerChannel);
   }

   if (toString(factOrData.tag, 4) != "data") {
      string tag{toString(factOrData.tag, 4)};
      rLog(channel, "%s ERROR: unknown tag>%s<", name.c_str(), tag.c_str());
      return;
   }
}

void WavReader::writeSnippet(
      const string& name, istream& file, ostream& out,
      FormatSubchunk& formatSubchunk,
      DataChunk& dataChunk,
      char* data
      ) {
   // ...
   uint32_t secondsDesired{10};
   if (formatSubchunk.bitsPerSample == 0) formatSubchunk.bitsPerSample = 8;
   uint32_t bytesPerSample{formatSubchunk.bitsPerSample / uint32_t{8}};

   uint32_t samplesToWrite{secondsDesired * formatSubchunk.samplesPerSecond};
   uint32_t totalSamples{dataChunk.length / bytesPerSample};

   samplesToWrite = min(samplesToWrite, totalSamples);

   uint32_t totalSeconds { totalSamples / formatSubchunk.samplesPerSecond };

   rLog(channel, "total seconds %u ", totalSeconds);

   dataChunk.length = dataLength(
         samplesToWrite, 
         bytesPerSample, 
         formatSubchunk.channels);
   out.write(reinterpret_cast<char*>(&dataChunk), sizeof(DataChunk));

   uint32_t startingSample{
      totalSeconds >= 10 ? 10 * formatSubchunk.samplesPerSecond : 0};

   writeSamples(&out, data, startingSample, samplesToWrite, bytesPerSample);

   rLog(channel, "completed writing %s", name.c_str());

   auto fileSize = fileUtil_->size(name);

   descriptor_->add(dest_, name, 
         totalSeconds, formatSubchunk.samplesPerSecond, formatSubchunk.channels,
         fileSize);
   
   //out.close(); // ostreams are RAII
}

uint32_t WavReader::dataLength(
      uint32_t samples, 
      uint32_t bytesPerSample,
      uint32_t channels
      ) const {
   return samples * bytesPerSample * channels;
}

void WavReader::writeSamples(ostream* out, char* data, 
      uint32_t startingSample, 
      uint32_t samplesToWrite, 
      uint32_t bytesPerSample,
      uint32_t channels) {
   rLog(channel, "writing %i samples", samplesToWrite);

   for (auto sample = startingSample; 
        sample < startingSample + samplesToWrite; 
        sample++) {
      auto byteOffsetForSample = sample * bytesPerSample * channels;
      for (uint32_t channel{0}; channel < channels; channel++) {
         auto byteOffsetForChannel =
            byteOffsetForSample + (channel * bytesPerSample);
         for (uint32_t byte{0}; byte < bytesPerSample; byte++) 
            out->put(data[byteOffsetForChannel + byte]);
      }
   }
}

void WavReader::seekToEndOfHeader(ifstream& file, int subchunkSize) {
   auto bytes = subchunkSize - sizeof(FormatSubchunk) + 1;
   file.seekg(bytes, ios_base::cur);
}

