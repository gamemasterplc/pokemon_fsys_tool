#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdint.h>
#include <nlohmann/json.hpp>

#define FSYS_V1_MAX_TYPE 19
#define FSYS_EXTERNAL_FILE_FLAG 0x80000000
#define FILE_COMPRESS_FLAG 0x80000000

//LZSS constants
#define N                4096   /* size of ring buffer */
#define F                  18   /* upper limit for match_length */
#define THRESHOLD       2   /* encode string into position and length
												   if match_length is greater than this */
#define NIL                     N       /* index for root of binary search trees */

struct fsys_header_data {
	uint32_t magic;
	uint32_t version;
	uint32_t archive_id;
	uint32_t num_files;
	uint32_t flags;
	uint32_t unk;
	uint32_t ofs_table;
	uint32_t data_start;
	uint32_t fsys_size;
};

struct fsys_offsets_data {
	uint32_t file_list_ofs;
	uint32_t str_ofs;
	uint32_t data_ofs;
};

struct fsys_file_data {
	uint32_t id;
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
	uint32_t unk;
	uint32_t packed_size;
	uint32_t unk2;
	uint32_t archive_name_ofs;
	uint32_t type;
	uint32_t name_ofs;
};

struct FSYSFile {
	uint32_t id;
	uint32_t offset;
	std::vector<uint8_t> data;
	std::vector<uint8_t> packed_data;
	bool compressed;
	uint32_t type;
	std::string name;
};

struct FileTypeInfo {
	uint32_t type_id;
	std::string name;
	std::string extension;
};

std::vector<FileTypeInfo> known_file_types = {
	{ 0, "sound_data", "bin" },
	{ 1, "room_data", "rdat" },
	{ 2, "object", "dat" },
	{ 3, "collision", "ccd" },
	{ 4, "music", "samp" },
	{ 5, "message", "msg" },
	{ 6, "font", "fnt" },
	{ 7, "script", "scd" },
	{ 9, "texture", "gtx" },
	{ 10, "particle", "gpt1" },
	{ 12, "camera", "cam" },
	{ 14, "code", "rel" },
	{ 15, "trainer_model", "pkx" },
	{ 16, "effect", "wzx" },
	{ 17, "gfl", "gfl" },
	{ 18, "battle_particle", "gpt1" },
	{ 19, "battle_code", "rel" },
	{ 20, "music_stream_header", "isf" },
	{ 21, "music_stream_data", "isfd" },
	{ 22, "movie_header", "thp" },
	{ 23, "movie_data", "thpd" },
	{ 24, "multi_texture", "gsw" },
	{ 25, "anim_texture", "atx" },
	{ 26, "binary", "bin" },
};

uint32_t fsys_version;
uint32_t fsys_archive_id;
std::vector<FSYSFile> fsys_files;

//LZSS variables
uint8_t text_buf[N + F - 1];    /* ring buffer of size N, with extra F-1 bytes to facilitate string comparison */
int match_position, match_length;  /* of longest match.  These are set by the InsertNode() procedure. */
int lson[N + 1], rson[N + 257], dad[N + 1];  /* left & right children & parents -- These constitute binary search trees. */

FileTypeInfo *GetFileTypeID(uint32_t id)
{
	for (size_t i = 0; i < known_file_types.size(); i++) {
		if (known_file_types[i].type_id == id) {
			return &known_file_types[i];
		}
	}
	return nullptr;
}

FileTypeInfo *GetFileTypeName(std::string name)
{
	for (size_t i = 0; i < known_file_types.size(); i++) {
		if (known_file_types[i].name == name) {
			return &known_file_types[i];
		}
	}
	return nullptr;
}

std::string GetFSYSFileName(const FSYSFile &file)
{
	FileTypeInfo *type_info = GetFileTypeID(file.type);
	if (!type_info) {
		return file.name + ".bin";
	}
	return file.name + "." + type_info->extension;
}

void to_json(nlohmann::ordered_json &j, const FSYSFile &file)
{
	FileTypeInfo *type_info = GetFileTypeID(file.type);
	if (!type_info) {
		std::cout << "Invalid file type " << file.type << std::endl;
		exit(1);
	}
	j = nlohmann::ordered_json{
		{ "id", file.id },
		{ "name", file.name },
		{ "type", type_info->name },
		{ "compressed", file.compressed }
	};
}

void from_json(const nlohmann::ordered_json &j, FSYSFile &file)
{

}

uint32_t ReadMemoryBufU32(uint8_t *buf)
{
	//Convert 4 bytes into native endian 32-bit word
	return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

uint32_t ReadFileU32(FILE *file)
{
	uint8_t temp[4];
	if (fread(&temp[0], 4, 1, file) != 1) {
		std::cout << "Failed to read from file." << std::endl;
		exit(1);
	}
	return ReadMemoryBufU32(temp);
}

std::string ReadFileString(FILE *file)
{
	uint8_t temp_char;
	std::string string;
	do {
		if (fread(&temp_char, 1, 1, file) != 1) {
			std::cout << "Failed to read from file." << std::endl;
			exit(1);
		}
		if (temp_char != 0) {
			string.push_back(temp_char);
		}
	} while (temp_char != 0);
	return string;
}

void WriteMemoryBufU32(uint8_t *buf, uint32_t value)
{
	//Split value into bytes in big-endian order
	buf[0] = value >> 24;
	buf[1] = (value >> 16) & 0xFF;
	buf[2] = (value >> 8) & 0xFF;
	buf[3] = value & 0xFF;
}

void WriteFileU32(FILE *file, uint32_t value)
{
	uint8_t temp[4];
	WriteMemoryBufU32(temp, value);
	//Write it
	fwrite(temp, 4, 1, file);
}

void WriteFileString(FILE *file, std::string string)
{
	fwrite(string.c_str(), string.length() + 1, 1, file);
}

void AlignU32(uint32_t &value, uint32_t to)
{
	while (value % to) {
		value++;
	}
}

void AlignFile(FILE *file, uint32_t alignment)
{
	uint32_t ofs = ftell(file);
	while (ofs % alignment) {
		//Write zero until file is aligned
		uint8_t zero = 0;
		fwrite(&zero, 1, 1, file);
		ofs++;
	}
}

void PackFSYS(std::string in_file, std::string out_file)
{
	
}

void ReadFSYSHeader(FILE *file, fsys_header_data &header)
{
	fseek(file, 0, SEEK_SET);
	header.magic = ReadFileU32(file);
	header.version = ReadFileU32(file);
	header.archive_id = ReadFileU32(file);
	header.num_files = ReadFileU32(file);
	header.flags = ReadFileU32(file);
	header.unk = ReadFileU32(file);
	header.ofs_table = ReadFileU32(file);
	header.data_start = ReadFileU32(file);
	header.fsys_size = ReadFileU32(file);
}

void ReadOffsetTable(FILE *file, uint32_t offset, fsys_offsets_data &table)
{
	fseek(file, offset, SEEK_SET);
	table.file_list_ofs = ReadFileU32(file);
	table.str_ofs = ReadFileU32(file);
	table.data_ofs = ReadFileU32(file);
}

void DecodeLZSS(uint8_t *dst, uint8_t *src)
{
	uint32_t dst_pos = 0;
	size_t text_buf_pos = N - F;
	uint32_t flag = 0;
	uint32_t magic = ReadMemoryBufU32(&src[0]);
	uint32_t out_size = ReadMemoryBufU32(&src[4]);
	uint32_t in_size = ReadMemoryBufU32(&src[8]);
	if (magic != 'LZSS') {
		std::cout << "Invalid LZSS data." << std::endl;
		exit(1);
	}
	src += 16;
	in_size -= 16;
	memset(text_buf, 0, N+F);
	while (dst_pos < out_size) {
		if (!(flag & 0x100)) {
			uint8_t value = *src++;
			flag = 0xFF00 | value;
		}
		if (flag & 0x1) {
			uint8_t value = *src++;
			text_buf[text_buf_pos] = dst[dst_pos++] = value;
			text_buf_pos = (text_buf_pos + 1) % N;
		} else {
			uint8_t byte1 = *src++;
			uint8_t byte2 = *src++;
			size_t ofs = ((byte2 & 0xF0) << 4) | byte1;
			size_t copy_size = (byte2 & 0xF) + THRESHOLD + 1;
			for (size_t i = 0; i < copy_size; i++) {
				dst[dst_pos++] = text_buf[text_buf_pos] = text_buf[ofs];
				ofs = (ofs + 1) % N;
				text_buf_pos = (text_buf_pos + 1) % N;
			}
		}
		flag >>= 1;
	}
}
void ReadFSYSFile(FILE *file, uint32_t file_ofs, FSYSFile &file_info)
{
	fsys_file_data data;
	fseek(file, file_ofs, SEEK_SET);
	data.id = ReadFileU32(file);
	data.offset = ReadFileU32(file);
	data.size = ReadFileU32(file);
	data.flags = ReadFileU32(file);
	data.unk = ReadFileU32(file);
	data.packed_size = ReadFileU32(file);
	data.unk2 = ReadFileU32(file);
	data.archive_name_ofs = ReadFileU32(file);
	data.type = ReadFileU32(file);
	data.name_ofs = ReadFileU32(file);
	file_info.id = data.id;
	file_info.offset = data.offset;
	if (data.flags & FILE_COMPRESS_FLAG) {
		file_info.compressed = true;
	} else {
		file_info.compressed = false;
	}
	file_info.type = data.type;
	fseek(file, data.name_ofs, SEEK_SET);
	file_info.name = ReadFileString(file);
	file_info.data.resize(data.size);
	if (file_info.compressed) {
		file_info.packed_data.resize(data.packed_size);
		fseek(file, data.offset, SEEK_SET);
		fread(&file_info.packed_data[0], data.packed_size, 1, file);
		DecodeLZSS(&file_info.data[0], &file_info.packed_data[0]);
	} else {
		fseek(file, data.offset, SEEK_SET);
		fread(&file_info.data[0], data.size, 1, file);
	}
}

void ReadFSYSFiles(FILE *file, uint32_t file_list_ofs, uint32_t num_files)
{
	for (uint32_t i = 0; i < num_files; i++) {
		FSYSFile file_info;
		fseek(file, file_list_ofs + (i * sizeof(uint32_t)), SEEK_SET);
		ReadFSYSFile(file, ReadFileU32(file), file_info);
		fsys_files.push_back(file_info);
	}
}

void ReadFSYS(std::string in_file)
{
	FILE *file = fopen(in_file.c_str(), "rb");
	if (!file) {
		std::cout << "Failed to open " << in_file << " for reading." << std::endl;
		exit(1);
	}
	fsys_header_data header;
	fsys_offsets_data offset_table;
	ReadFSYSHeader(file, header);
	if (header.magic != 'FSYS') {
		std::cout << "Invalid header magic." << std::endl;
		exit(1);
	}
	if (!(header.flags & FSYS_EXTERNAL_FILE_FLAG)) {
		std::cout << "FSYS files that use external files which are not supported." << std::endl;
		exit(1);
	}
	ReadOffsetTable(file, header.ofs_table, offset_table);
	fsys_archive_id = header.archive_id;
	fsys_version = header.version;
	ReadFSYSFiles(file, offset_table.file_list_ofs, header.num_files);
	fclose(file);
}

void DumpFSYS(std::string base_path)
{
	nlohmann::ordered_json json;
	json["version"] = fsys_version;
	json["id"] = fsys_archive_id;
	json["files"] = fsys_files;
	std::cout << json.dump(4);
}

void UnpackFSYS(std::string in_file, std::string base_path)
{
	ReadFSYS(in_file);
	DumpFSYS(base_path);
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		std::cout << "Usage: " << argv[0] << " [p/u] input output" << std::endl;
		std::cout << "p is used in the second argument when packing the input XML into an output FSYS file." << std::endl;
		std::cout << "u is used in the second argument when dumping an input FSYS file into a base path." << std::endl;
		return 1;
	}
	if (argv[1][0] == 'p') {
		PackFSYS(argv[2], argv[3]);
	} else if (argv[1][0] == 'u') {
		UnpackFSYS(argv[2], argv[3]);
	} else {
		std::cout << "Invalid second argument " << argv[1] << std::endl;
		return 1;
	}
	return 0;
}
