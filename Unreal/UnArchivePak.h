#ifndef __UNARCHIVE_PAK_H__
#define __UNARCHIVE_PAK_H__

#if UNREAL4


// NOTE: this implementation has a lot of common things with FObbFile. If there'll be another
// one virtual file system with similar implementation, it's worth to make some parent class
// for all of them which will differs perhaps only with AttachReader() method.

#define PAK_FILE_MAGIC		0x5A6F12E1

// Pak file versions
enum
{
	PAK_INITIAL = 1,
	PAK_NO_TIMESTAMPS = 2,
	PAK_COMPRESSION_ENCRYPTION = 3,			// UE4.3+
	PAK_INDEX_ENCRYPTION = 4,				// UE4.17+ - encrypts only pak file index data leaving file content as is
	PAK_RELATIVE_CHUNK_OFFSETS = 5,			// UE4.20+
	PAK_DELETE_RECORDS = 6,					// UE4.21+ - this constant is not used in UE4 code
	PAK_ENCRYPTION_KEY_GUID = 7,			// ... allows to use multiple encryption keys over the single project
	PAK_FNAME_BASED_COMPRESSION_METHOD = 8, // UE4.22+ - use string instead of enum for compression method

	PAK_LATEST_PLUS_ONE,
	PAK_LATEST = PAK_LATEST_PLUS_ONE - 1
};

// hack: use ArLicenseeVer to not pass FPakInfo.Version to serializer
#define PakVer		ArLicenseeVer

struct FPakInfo
{
	int32		Magic;
	int32		Version;
	int64		IndexOffset;
	int64		IndexSize;
	byte		IndexHash[20];
	// When new fields are added to FPakInfo, they're serialized before 'Magic' to keep compatibility
	// with older pak file versions. At the same time, structure size grows.
	byte		bEncryptedIndex;
	FGuid		EncryptionKeyGuid;

	enum { Size = sizeof(int32) * 2 + sizeof(int64) * 2 + 20 + /* new fields */ 1 + sizeof(FGuid)};

	friend FArchive& operator<<(FArchive& Ar, FPakInfo& P)
	{
		// New FPakInfo fields.
		Ar << P.EncryptionKeyGuid;			// PAK_ENCRYPTION_KEY_GUID
		Ar << P.bEncryptedIndex;			// PAK_INDEX_ENCRYPTION

		// Old FPakInfo fields.
		Ar << P.Magic << P.Version << P.IndexOffset << P.IndexSize;
		Ar.Serialize(ARRAY_ARG(P.IndexHash));

		// Reset new fields to their default states when seralizing older pak format.
		if (P.Version < PAK_INDEX_ENCRYPTION)
		{
			P.bEncryptedIndex = false;
		}
		if (P.Version < PAK_ENCRYPTION_KEY_GUID)
		{
			P.EncryptionKeyGuid = { 0, 0, 0, 0 };
		}
		return Ar;
	}
};

struct FPakCompressedBlock
{
	int64		CompressedStart;
	int64		CompressedEnd;

	friend FArchive& operator<<(FArchive& Ar, FPakCompressedBlock& B)
	{
		return Ar << B.CompressedStart << B.CompressedEnd;
	}
};

struct FPakEntry
{
	const char*	Name;
	int64		Pos;
	int64		Size;
	int64		UncompressedSize;
	int32		CompressionMethod;
	byte		Hash[20];
	byte		bEncrypted;					// replaced with 'Flags' in UE4.21
	TArray<FPakCompressedBlock> CompressionBlocks;
	int32		CompressionBlockSize;

	int32		StructSize;					// computed value
	FPakEntry*	HashNext;					// computed value

	friend FArchive& operator<<(FArchive& Ar, FPakEntry& P)
	{
		guard(FPakEntry<<);

		// FPakEntry is duplicated before each stored file, without a filename. So,
		// remember the serialized size of this structure to avoid recomputation later.
		int64 StartOffset = Ar.Tell64();

#if GEARS4
		if (GForceGame == GAME_Gears4)
		{
			Ar << P.Pos << (int32&)P.Size << (int32&)P.UncompressedSize << (byte&)P.CompressionMethod;
			if (Ar.PakVer < PAK_NO_TIMESTAMPS)
			{
				int64 timestamp;
				Ar << timestamp;
			}
			if (Ar.PakVer >= PAK_COMPRESSION_ENCRYPTION)
			{
				if (P.CompressionMethod != 0)
					Ar << P.CompressionBlocks;
				Ar << P.CompressionBlockSize;
				if (P.CompressionMethod == 4)
					P.CompressionMethod = COMPRESS_LZ4;
			}
			goto end;
		}
#endif // GEARS4

		Ar << P.Pos << P.Size << P.UncompressedSize << P.CompressionMethod;

		if (Ar.PakVer < PAK_NO_TIMESTAMPS)
		{
			int64 timestamp;
			Ar << timestamp;
		}

		Ar.Serialize(ARRAY_ARG(P.Hash));

		if (Ar.PakVer >= PAK_COMPRESSION_ENCRYPTION)
		{
			if (P.CompressionMethod != 0)
				Ar << P.CompressionBlocks;
			Ar << P.bEncrypted << P.CompressionBlockSize;
		}
#if TEKKEN7
		if (GForceGame == GAME_Tekken7)
			P.bEncrypted = false;		// Tekken 7 has 'bEncrypted' flag set, but actually there's no encryption
#endif

		if (Ar.PakVer >= PAK_RELATIVE_CHUNK_OFFSETS)
		{
			// Convert relative compressed offsets to absolute
			for (int i = 0; i < P.CompressionBlocks.Num(); i++)
			{
				FPakCompressedBlock& B = P.CompressionBlocks[i];
				B.CompressedStart += P.Pos;
				B.CompressedEnd += P.Pos;
			}
		}

	end:
		P.StructSize = Ar.Tell64() - StartOffset;

		return Ar;

		unguard;
	}
};

inline bool PakRequireAesKey(bool fatal = true)
{
	if ((GAesKey.Len() == 0) && !UE4EncryptedPak())
	{
		if (fatal)
			appError("AES key is required");
		return false;
	}
	return true;
}

class FPakFile : public FArchive
{
	DECLARE_ARCHIVE(FPakFile, FArchive);
public:
	FPakFile(const FPakEntry* info, FArchive* reader)
	:	Info(info)
	,	Reader(reader)
	,	UncompressedBuffer(NULL)
	{}

	virtual ~FPakFile()
	{
		if (UncompressedBuffer)
			appFree(UncompressedBuffer);
	}

	virtual void Serialize(void *data, int size)
	{
		guard(FPakFile::Serialize);
		if (ArStopper > 0 && ArPos + size > ArStopper)
			appError("Serializing behind stopper (%X+%X > %X)", ArPos, size, ArStopper);

		if (Info->CompressionMethod)
		{
			guard(SerializeCompressed);

			while (size > 0)
			{
				if ((UncompressedBuffer == NULL) || (ArPos < UncompressedBufferPos) || (ArPos >= UncompressedBufferPos + Info->CompressionBlockSize))
				{
					// buffer is not ready
					if (UncompressedBuffer == NULL)
					{
						UncompressedBuffer = (byte*)appMalloc((int)Info->CompressionBlockSize); // size of uncompressed block
					}
					// prepare buffer
					int BlockIndex = ArPos / Info->CompressionBlockSize;
					UncompressedBufferPos = Info->CompressionBlockSize * BlockIndex;

					const FPakCompressedBlock& Block = Info->CompressionBlocks[BlockIndex];
					int CompressedBlockSize = (int)(Block.CompressedEnd - Block.CompressedStart);
					int UncompressedBlockSize = min((int)Info->CompressionBlockSize, (int)Info->UncompressedSize - UncompressedBufferPos); // don't pass file end
					byte* CompressedData;
					if (!Info->bEncrypted)
					{
						CompressedData = (byte*)appMalloc(CompressedBlockSize);
						Reader->Seek64(Block.CompressedStart);
						Reader->Serialize(CompressedData, CompressedBlockSize);
					}
					else
					{
						int EncryptedSize = Align(CompressedBlockSize, EncryptionAlign);
						CompressedData = (byte*)appMalloc(EncryptedSize);
						Reader->Seek64(Block.CompressedStart);
						Reader->Serialize(CompressedData, EncryptedSize);
						PakRequireAesKey();
						appDecryptAES(CompressedData, EncryptedSize);
					}
					appDecompress(CompressedData, CompressedBlockSize, UncompressedBuffer, UncompressedBlockSize, Info->CompressionMethod);
					appFree(CompressedData);
				}

				// data is in buffer, copy it
				int BytesToCopy = UncompressedBufferPos + Info->CompressionBlockSize - ArPos; // number of bytes until end of the buffer
				if (BytesToCopy > size) BytesToCopy = size;
				assert(BytesToCopy > 0);

				// copy uncompressed data
				int OffsetInBuffer = ArPos - UncompressedBufferPos;
				memcpy(data, UncompressedBuffer + OffsetInBuffer, BytesToCopy);

				// advance pointers
				ArPos += BytesToCopy;
				size  -= BytesToCopy;
				data  = OffsetPointer(data, BytesToCopy);
			}

			unguard;
		}
		else if (Info->bEncrypted)
		{
			guard(SerializeEncrypted);

			// Uncompressed encrypted data. Reuse compression fields to handle decryption efficiently
			if (UncompressedBuffer == NULL)
			{
				UncompressedBuffer = (byte*)appMalloc(EncryptedBufferSize);
				UncompressedBufferPos = 0x40000000; // some invalid value
			}
			while (size > 0)
			{
				if ((ArPos < UncompressedBufferPos) || (ArPos >= UncompressedBufferPos + EncryptedBufferSize))
				{
					// Should fetch block and decrypt it.
					// Note: AES is block encryption, so we should always align read requests for correct decryption.
					UncompressedBufferPos = ArPos & ~(EncryptionAlign - 1);
					Reader->Seek64(Info->Pos + Info->StructSize + UncompressedBufferPos);
					int RemainingSize = Info->Size - UncompressedBufferPos;
					if (RemainingSize > EncryptedBufferSize)
						RemainingSize = EncryptedBufferSize;
					RemainingSize = Align(RemainingSize, EncryptionAlign); // align for AES, pak contains aligned data
					Reader->Serialize(UncompressedBuffer, RemainingSize);
					PakRequireAesKey();
					appDecryptAES(UncompressedBuffer, RemainingSize);
				}

				// Now copy decrypted data from UncompressedBuffer (code is very similar to those used in decompression above)
				int BytesToCopy = UncompressedBufferPos + EncryptedBufferSize - ArPos; // number of bytes until end of the buffer
				if (BytesToCopy > size) BytesToCopy = size;
				assert(BytesToCopy > 0);

				// copy uncompressed data
				int OffsetInBuffer = ArPos - UncompressedBufferPos;
				memcpy(data, UncompressedBuffer + OffsetInBuffer, BytesToCopy);

				// advance pointers
				ArPos += BytesToCopy;
				size  -= BytesToCopy;
				data  = OffsetPointer(data, BytesToCopy);
			}

			unguard;
		}
		else
		{
			guard(SerializeUncompressed);

			// Pure data
			// seek every time in a case if the same 'Reader' was used by different FPakFile
			// (this is a lightweight operation for buffered FArchive)
			Reader->Seek64(Info->Pos + Info->StructSize + ArPos);
			Reader->Serialize(data, size);
			ArPos += size;

			unguard;
		}
		unguardf("file=%s", Info->Name);
	}

	virtual void Seek(int Pos)
	{
		guard(FPakFile::Seek);
		assert(Pos >= 0 && Pos < Info->UncompressedSize);
		ArPos = Pos;
		unguardf("file=%s", Info->Name);
	}

	virtual int GetFileSize() const
	{
		return (int)Info->UncompressedSize;
	}

	virtual void Close()
	{
		if (UncompressedBuffer)
		{
			appFree(UncompressedBuffer);
			UncompressedBuffer = NULL;
		}
	}

protected:
	const FPakEntry* Info;
	FArchive*	Reader;
	byte*		UncompressedBuffer;
	int			UncompressedBufferPos;

	enum { EncryptionAlign = 16 }; // AES-specific constant
	enum { EncryptedBufferSize = 256 }; //?? TODO: check - may be value 16 will be better for performance
};


class FPakVFS : public FVirtualFileSystem
{
public:
	FPakVFS(const char* InFilename)
	:	Filename(InFilename)
	,	Reader(NULL)
	,	LastInfo(NULL)
	,	HashTable(NULL)
	{}

	virtual ~FPakVFS()
	{
		delete Reader;
		if (HashTable) delete[] HashTable;
	}

	void CompactFilePath(FString& Path)
	{
		guard(FPakVFS::CompactFilePath);

		if (Path.StartsWith("/Engine/Content"))	// -> /Engine
		{
			Path.RemoveAt(7, 8);
			return;
		}
		if (Path.StartsWith("/Engine/Plugins")) // -> /Plugins
		{
			Path.RemoveAt(0, 7);
			return;
		}

		if (Path[0] != '/')
			return;

		char* delim = strchr(&Path[1], '/');
		if (!delim)
			return;
		if (strncmp(delim, "/Content/", 9) != 0)
			return;

		int pos = delim - &Path[0];
		if (pos > 4)
		{
			// /GameName/Content -> /Game
			int toRemove = pos + 8 - 5;
			Path.RemoveAt(5, toRemove);
			memcpy(&Path[1], "Game", 4);
		}

		unguard;
	}

	virtual bool AttachReader(FArchive* reader, FString& error)
	{
		int guardVersion = 0; // used for some details in a case of crash
		guard(FPakVFS::ReadDirectory);

		// Read pak header
		int64 HeaderOffset = reader->GetFileSize64() - FPakInfo::Size;
		if (HeaderOffset <= 0)
		{
			// The file is too small
			return false;
		}
		reader->Seek64(HeaderOffset);

		FPakInfo info;
		*reader << info;
		if (info.Magic != PAK_FILE_MAGIC)		// no endian checking here
			return false;

		//!! TODO
		if (info.Version >= PAK_FNAME_BASED_COMPRESSION_METHOD)
		{
			appError("UE4.22 TODO: pak version 8 (need samples)");
		}

		guardVersion = info.Version;
		if (info.Version > PAK_LATEST)
		{
			appPrintf("WARNING: Pak file \"%s\" has unsupported version %d\n", *Filename, info.Version);
		}

		if (info.bEncryptedIndex)
		{
			if (!PakRequireAesKey(false))
			{
				char buf[1024];
				appSprintf(ARRAY_ARG(buf), "WARNING: Pak \"%s\" has encrypted index. Skipping.", *Filename);
				error = buf;
				return false;
			}
		}

		// Read pak index

		reader->ArLicenseeVer = info.Version;

		reader->Seek64(info.IndexOffset);

		// Manage pak files with encrypted index
		FMemReader* InfoReaderProxy = NULL;
		byte* InfoBlock = NULL;
		FArchive* InfoReader = reader;

		if (info.bEncryptedIndex)
		{
			guard(CheckEncryptedIndex);

			InfoBlock = new byte[info.IndexSize];
			reader->Serialize(InfoBlock, info.IndexSize);
			appDecryptAES(InfoBlock, info.IndexSize);
			InfoReaderProxy = new FMemReader(InfoBlock, info.IndexSize);
			InfoReaderProxy->SetupFrom(*reader);
			InfoReader = InfoReaderProxy;

			// Try to validate the decrypted data. The first thing going here is MountPoint which is FString.
			int32 StringLen;
			*InfoReader << StringLen;
			bool bFail = false;
			if (StringLen > 512 || StringLen < -512)
			{
				bFail = true;
			}
			if (!bFail)
			{
				// Seek to terminating zero character
				if (StringLen < 0)
				{
					InfoReader->Seek(InfoReader->Tell() - (StringLen - 1) * 2);
					uint16 c;
					*InfoReader << c;
					bFail = (c != 0);
				}
				else
				{
					InfoReader->Seek(InfoReader->Tell() + StringLen - 1);
					char c;
					*InfoReader << c;
					bFail = (c != 0);
				}
			}
			if (bFail)
			{
				delete[] InfoBlock;
				delete InfoReaderProxy;
				char buf[1024];
				appSprintf(ARRAY_ARG(buf), "WARNING: The provided encryption key doesn't work with \"%s\". Skipping.", *Filename);
				error = buf;
				return false;
			}

			// Data is ok, seek to data start.
			InfoReader->Seek(0);

			unguard;
		}

		// this file looks correct, store 'reader'
		Reader = reader;

		// Read pak index

		FStaticString<MAX_PACKAGE_PATH> MountPoint;

		TRY {
			// Read MountPoint with catching error, to override error message.
			*InfoReader << MountPoint;
		} CATCH {
			if (info.bEncryptedIndex)
			{
				// Display nice error message
				appError("Error during loading of encrypted pak file index. Probably the provided AES key is not correct.");
			}
			else
			{
				THROW_AGAIN;
			}
		}

		// Process MountPoint
		bool badMountPoint = false;
		if (!MountPoint.RemoveFromStart("../../.."))
			badMountPoint = true;
		if (MountPoint[0] != '/' || ( (MountPoint.Len() > 1) && (MountPoint[1] == '.') ))
			badMountPoint = true;

		if (badMountPoint)
		{
			appNotify("WARNING: Pak \"%s\" has strange mount point \"%s\", mounting to root", *Filename, *MountPoint);
			MountPoint = "/";
		}

		int count;
		*InfoReader << count;
		FileInfos.AddZeroed(count);

		int numEncryptedFiles = 0;
		for (int i = 0; i < count; i++)
		{
			guard(ReadInfo);

			FPakEntry& E = FileInfos[i];
			// serialize name, combine with MountPoint
			FStaticString<MAX_PACKAGE_PATH> Filename;
			*InfoReader << Filename;
			FStaticString<MAX_PACKAGE_PATH> CombinedPath;
			CombinedPath = MountPoint;
			CombinedPath += Filename;
			// compact file name
			CompactFilePath(CombinedPath);
			// allocate file name in pool
			E.Name = appStrdupPool(*CombinedPath);
			// serialize other fields
			*InfoReader << E;
			if (E.bEncrypted)
			{
//				appPrintf("Encrypted file: %s\n", *Filename);
				numEncryptedFiles++;
			}

			unguard("Index=%d/%d", i, count);
		}
		if (count >= MIN_PAK_SIZE_FOR_HASHING)
		{
			// Hash everything
			for (int i = 0; i < count; i++)
			{
				AddFileToHash(&FileInfos[i]);
			}
		}
		// Cleanup
		if (InfoBlock)
		{
			delete[] InfoBlock;
			delete InfoReaderProxy;
		}

		// Print statistics
		appPrintf("Pak %s: %d files", *Filename, count);
		if (numEncryptedFiles)
			appPrintf(" (%d encrypted)", numEncryptedFiles);
		if (strcmp(*MountPoint, "/") != 0)
			appPrintf(", mount point: \"%s\"", *MountPoint);
		appPrintf(", version %d\n", info.Version);

		return true;

		unguardf("PakVer=%d", guardVersion);
	}

	virtual int GetFileSize(const char* name)
	{
		const FPakEntry* info = FindFile(name);
		return (info) ? (int)info->UncompressedSize : 0;
	}

	// iterating over all files
	virtual int NumFiles() const
	{
		return FileInfos.Num();
	}

	virtual const char* FileName(int i)
	{
		FPakEntry* info = &FileInfos[i];
		LastInfo = info;
		return info->Name;
	}

	virtual FArchive* CreateReader(const char* name)
	{
		const FPakEntry* info = FindFile(name);
		if (!info) return NULL;
/*		if (info->bEncrypted)
		{
			appPrintf("pak(%s): attempt to open encrypted file %s\n", *Filename, name);
			return NULL;
		} */
		return new FPakFile(info, Reader);
	}

protected:
	enum { HASH_SIZE = 1024 };
	enum { HASH_MASK = HASH_SIZE - 1 };
	enum { MIN_PAK_SIZE_FOR_HASHING = 256 };

	FString				Filename;
	FArchive*			Reader;
	TArray<FPakEntry>	FileInfos;
	FPakEntry*			LastInfo;			// cached last accessed file info, simple optimization
	FPakEntry**			HashTable;

	static uint16 GetHashForFileName(const char* FileName)
	{
		uint16 hash = 0;
		while (char c = *FileName++)
		{
			if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; // lowercase a character
			hash = ROL16(hash, 5) - hash + ((c << 4) + c ^ 0x13F);	// some crazy hash function
		}
		hash &= HASH_MASK;
		return hash;
	}

	void AddFileToHash(FPakEntry* File)
	{
		if (!HashTable)
		{
			HashTable = new FPakEntry* [HASH_SIZE];
			memset(HashTable, 0, sizeof(FPakEntry*) * HASH_SIZE);
		}
		uint16 hash = GetHashForFileName(File->Name);
		File->HashNext = HashTable[hash];
		HashTable[hash] = File;
	}

	const FPakEntry* FindFile(const char* name)
	{
		if (LastInfo && !stricmp(LastInfo->Name, name))
			return LastInfo;

		if (HashTable)
		{
			// Have a hash table, use it
			uint16 hash = GetHashForFileName(name);
			for (FPakEntry* info = HashTable[hash]; info; info = info->HashNext)
			{
				if (!stricmp(info->Name, name))
				{
					LastInfo = info;
					return info;
				}
			}
			return NULL;
		}

		// Linear search without a hash table
		for (int i = 0; i < FileInfos.Num(); i++)
		{
			FPakEntry* info = &FileInfos[i];
			if (!stricmp(info->Name, name))
			{
				LastInfo = info;
				return info;
			}
		}
		return NULL;
	}
};


#endif // UNREAL4

#endif // __UNARCHIVE_PAK_H__
