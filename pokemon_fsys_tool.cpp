#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <fstream>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
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
			if (fsys_version < 0x200 && known_file_types[i].type_id > FSYS_V1_MAX_TYPE) {
				return nullptr;
			}
			return &known_file_types[i];
		}
	}
	return nullptr;
}

FileTypeInfo *GetFileTypeName(std::string name)
{
	for (size_t i = 0; i < known_file_types.size(); i++) {
		if (known_file_types[i].name == name) {
			if (fsys_version < 0x200 && known_file_types[i].type_id > FSYS_V1_MAX_TYPE) {
				return nullptr;
			}
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
		std::cout << "Invalid file type value " << file.type << std::endl;
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
	std::string type_name;
	FileTypeInfo *type_info;
	j.at("id").get_to(file.id);
	j.at("name").get_to(file.name);
	j.at("type").get_to(type_name);
	file.offset = 0;
	file.compressed = j.value("compressed", false);
	type_info = GetFileTypeName(type_name);
	if (!type_info) {
		std::cout << "Invalid file type name " << type_name << std::endl;
		exit(1);
	}
	file.type = type_info->type_id;
}

bool MakeDirectory(std::string dir)
{
	int ret;
#if defined(_WIN32)
	ret = _mkdir(dir.c_str());
#else 
	ret = mkdir(dir.c_str(), 0777); // notice that 777 is different than 0777
#endif]
	return ret != -1 || errno == EEXIST;
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

void InitTree(void)  /* initialize trees */
{
	int  i;

	/* For i = 0 to N - 1, rson[i] and lson[i] will be the right and
	   left children of node i.  These nodes need not be initialized.
	   Also, dad[i] is the parent of node i.  These are initialized to
	   NIL (= N), which stands for 'not used.'
	   For i = 0 to 255, rson[N + i + 1] is the root of the tree
	   for strings that begin with character i.  These are initialized
	   to NIL.  Note there are 256 trees. */

	for (i = N + 1; i <= N + 256; i++) rson[i] = NIL;
	for (i = 0; i < N; i++) dad[i] = NIL;
}

void InsertNode(int r)
/* Inserts string of length F, text_buf[r..r+F-1], into one of the
   trees (text_buf[r]'th tree) and returns the longest-match position
   and length via the global variables match_position and match_length.
   If match_length = F, then removes the old node in favor of the new
   one, because the old one will be deleted sooner.
   Note r plays double role, as tree node and position in buffer. */
{
	int  i, p, cmp;
	uint8_t *key;

	cmp = 1;  key = &text_buf[r];  p = N + 1 + key[0];
	rson[r] = lson[r] = NIL;  match_length = 0;
	for (; ; ) {
		if (cmp >= 0) {
			if (rson[p] != NIL) p = rson[p];
			else { rson[p] = r;  dad[r] = p;  return; }
		} else {
			if (lson[p] != NIL) p = lson[p];
			else { lson[p] = r;  dad[r] = p;  return; }
		}
		for (i = 1; i < F; i++)
			if ((cmp = key[i] - text_buf[p + i]) != 0)  break;
		if (i > match_length) {
			match_position = p;
			if ((match_length = i) >= F)  break;
		}
	}
	dad[r] = dad[p];  lson[r] = lson[p];  rson[r] = rson[p];
	dad[lson[p]] = r;  dad[rson[p]] = r;
	if (rson[dad[p]] == p) rson[dad[p]] = r;
	else                   lson[dad[p]] = r;
	dad[p] = NIL;  /* remove p */
}

void DeleteNode(int p)  /* deletes node p from tree */
{
	int  q;

	if (dad[p] == NIL) return;  /* not in tree */
	if (rson[p] == NIL) q = lson[p];
	else if (lson[p] == NIL) q = rson[p];
	else {
		q = lson[p];
		if (rson[q] != NIL) {
			do { q = rson[q]; } while (rson[q] != NIL);
			rson[dad[q]] = lson[q];  dad[lson[q]] = dad[q];
			lson[q] = lson[p];  dad[lson[p]] = q;
		}
		rson[q] = rson[p];  dad[rson[p]] = q;
	}
	dad[q] = dad[p];
	if (rson[dad[p]] == p) rson[dad[p]] = q;  else lson[dad[p]] = q;
	dad[p] = NIL;
}

void CompressFSYSFile(FSYSFile &file)
{
	int  i, c, len, r, s, last_match_length, code_buf_ptr;
	uint8_t code_buf[17], mask;
	size_t src_pos = 0;
	uint32_t codesize = 16;

	file.packed_data.resize(16);
	WriteMemoryBufU32(&file.packed_data[0], 'LZSS');
	WriteMemoryBufU32(&file.packed_data[4], file.data.size());
	WriteMemoryBufU32(&file.packed_data[8], 0);
	WriteMemoryBufU32(&file.packed_data[12], 0);
	InitTree();  /* initialize trees */
	code_buf[0] = 0;  /* code_buf[1..16] saves eight units of code, and
			code_buf[0] works as eight flags, "1" representing that the unit
			is an unencoded letter (1 byte), "0" a position-and-length pair
			(2 bytes).  Thus, eight units require at most 16 bytes of code. */
	code_buf_ptr = mask = 1;
	s = 0;  r = N - F;
	for (i = s; i < r; i++) text_buf[i] = '\0';  /* Clear the buffer with
			any character that will appear often. */
	for (len = 0; len < F && src_pos < file.data.size(); len++)
		text_buf[r + len] = c = file.data[src_pos++];  /* Read F bytes into the last F bytes of
				the buffer */
	if (len == 0) return;  /* text of size zero */
	for (i = 1; i <= F; i++) InsertNode(r - i);  /* Insert the F strings,
			each of which begins with one or more 'space' characters.  Note
			the order in which these strings are inserted.  This way,
			degenerate trees will be less likely to occur. */
	InsertNode(r);  /* Finally, insert the whole string just read.  The
			global variables match_length and match_position are set. */
	do {
		if (match_length > len) match_length = len;  /* match_length
				may be spuriously long near the end of text. */
		if (match_length <= THRESHOLD) {
			match_length = 1;  /* Not long enough match.  Send one byte. */
			code_buf[0] |= mask;  /* 'send one byte' flag */
			code_buf[code_buf_ptr++] = text_buf[r];  /* Send uncoded. */
		} else {
			code_buf[code_buf_ptr++] = (uint8_t)match_position;
			code_buf[code_buf_ptr++] = (uint8_t)
				(((match_position >> 4) & 0xF0)
					| (match_length - (THRESHOLD + 1)));  /* Send position and
								  length pair. Note match_length > THRESHOLD. */
		}
		if ((mask <<= 1) == 0) {  /* Shift mask left one bit. */
			for (i = 0; i < code_buf_ptr; i++)  /* Send at most 8 units of */
				file.packed_data.push_back(code_buf[i]);    /* code together */
			codesize += code_buf_ptr;
			code_buf[0] = 0;  code_buf_ptr = mask = 1;
		}
		last_match_length = match_length;
		for (i = 0; i < last_match_length &&
			src_pos < file.data.size(); i++) {
			DeleteNode(s);          /* Delete old strings and */
			text_buf[s] = c = file.data[src_pos++];        /* read new bytes */
			if (s < F - 1) text_buf[s + N] = c;  /* If the position is
					near the end of buffer, extend the buffer to make
					string comparison easier. */
			s = (s + 1) & (N - 1);  r = (r + 1) & (N - 1);
			/* Since this is a ring buffer, increment the position
			   modulo N. */
			InsertNode(r);  /* Register the string in text_buf[r..r+F-1] */
		}
		while (i++ < last_match_length) {       /* After the end of text, */
			DeleteNode(s);                                  /* no need to read, but */
			s = (s + 1) & (N - 1);  r = (r + 1) & (N - 1);
			if (--len) InsertNode(r);               /* buffer may not be empty. */
		}
	} while (len > 0);      /* until length of string to be processed is zero */
	if (code_buf_ptr > 1) {         /* Send remaining code. */
		for (i = 0; i < code_buf_ptr; i++) file.packed_data.push_back(code_buf[i]);
		codesize += code_buf_ptr;
	}
	WriteMemoryBufU32(&file.packed_data[8], codesize);
}

void ReadJSON(std::string in_file)
{
	std::ifstream file(in_file);
	if (!file.is_open()) {
		std::cout << "Failed to open " << in_file << " for reading." << std::endl;
		exit(1);
	}
	try {
		nlohmann::ordered_json json = nlohmann::json::parse(file);
		fsys_version = json.value("version", 513);
		json.at("id").get_to(fsys_archive_id);
		json.at("files").get_to(fsys_files);
	} catch (nlohmann::json::exception &exception) {
		std::cout << exception.what() << std::endl;
		exit(1);
	}
}

void ReadFiles(std::string json_filename)
{
	size_t slash_pos = json_filename.find_last_of("\\/") + 1;
	size_t dot_pos = json_filename.find_last_of(".");
	std::string json_dir = json_filename.substr(0, slash_pos);
	std::string json_name = json_filename.substr(slash_pos, dot_pos - slash_pos);
	for (size_t i = 0; i < fsys_files.size(); i++) {
		std::string filename = json_dir + json_name + "/" + GetFSYSFileName(fsys_files[i]);
		FILE *file = fopen(filename.c_str(), "rb");
		if (!file) {
			std::cout << "Failed to open " << filename << " for writing." << std::endl;
			exit(1);
		}
		fseek(file, 0, SEEK_END);
		fsys_files[i].data.resize(ftell(file));
		fseek(file, 0, SEEK_SET);
		fread(&fsys_files[i].data[0], fsys_files[i].data.size(), 1, file);
		fclose(file);
	}
}

void CompressFiles()
{
	for (size_t i = 0; i < fsys_files.size(); i++) {
		if (fsys_files[i].compressed) {
			CompressFSYSFile(fsys_files[i]);
		}
	}
}

void PackFSYS(std::string in_file, std::string out_file)
{
	ReadJSON(in_file);
	ReadFiles(in_file);
	CompressFiles();
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
	std::string json_path = base_path + ".json";
	std::string out_dir_path = base_path + "/";
	std::ofstream json_file(json_path);
	nlohmann::ordered_json json;
	if (!MakeDirectory(out_dir_path)) {
		std::cout << "Failed to create " << out_dir_path << "." << std::endl;
		exit(1);
	}
	if (!json_file.is_open()) {
		std::cout << "Failed to open " << json_path << " for writing." << std::endl;
		exit(1);
	}
	json["version"] = fsys_version;
	json["id"] = fsys_archive_id;
	json["files"] = fsys_files;
	json_file << json.dump(4);
	for (size_t i = 0; i < fsys_files.size(); i++) {
		std::string filename = out_dir_path+GetFSYSFileName(fsys_files[i]);
		FILE *file = fopen(filename.c_str(), "wb");
		if (!file) {
			std::cout << "Failed to open " << filename << " for writing." << std::endl;
			exit(1);
		}
		fwrite(&fsys_files[i].data[0], fsys_files[i].data.size(), 1, file);
		fclose(file);
	}
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
		std::cout << "p is used in the second argument when packing the input JSON into an output FSYS file." << std::endl;
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
