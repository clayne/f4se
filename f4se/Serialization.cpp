#include "f4se/Serialization.h"
#include "common/IFileStream.h"
#include "f4se/PluginManager.h"
#include "f4se/GameAPI.h"
#include "f4se_common/f4se_version.h"
#include <vector>
#include <shlobj.h>
#include <io.h>
#include "f4se/GameData.h"
#include "f4se/InternalSerialization.h"
#include "f4se/GameSettings.h"
#include "f4se/ScaleformCallbacks.h"
#include "f4se/ScaleformValue.h"

namespace Serialization
{
	const char * kSavegamePath = "\\My Games\\Fallout4\\";

	// file format internals

	//	general format:
	//	Header			header
	//		PluginHeader	plugin[header.numPlugins]
	//			ChunkHeader		chunk[plugin.numChunks]
	//				UInt8			data[chunk.length]

	struct Header
	{
		enum
		{
			kSignature =		MACRO_SWAP32('F4SE'),	// endian-swapping so the order matches
			kVersion =			1,

			kVersion_Invalid =	0
		};

		UInt32	signature;
		UInt32	formatVersion;
		UInt32	f4seVersion;
		UInt32	runtimeVersion;
		UInt32	numPlugins;
	};

	struct PluginHeader
	{
		UInt32	signature;
		UInt32	numChunks;
		UInt32	length;		// length of following data including ChunkHeader
	};

	struct ChunkHeader
	{
		UInt32	type;
		UInt32	version;
		UInt32	length;
	};

	// locals

	std::string		s_savePath;
	IFileStream		s_currentFile;

	typedef std::vector <PluginCallbacks>	PluginCallbackList;
	PluginCallbackList	s_pluginCallbacks;

	PluginHandle	s_currentPlugin = 0;

	Header			s_fileHeader = { 0 };

	UInt64			s_pluginHeaderOffset = 0;
	PluginHeader	s_pluginHeader = { 0 };

	bool			s_chunkOpen = false;
	UInt64			s_chunkHeaderOffset = 0;
	ChunkHeader		s_chunkHeader = { 0 };

	// utilities

	// make full path from save name
	std::string MakeSavePath(std::string name, const char * extension)
	{
		char	path[MAX_PATH];
		ASSERT(SUCCEEDED(SHGetFolderPath(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, path)));

		std::string	result = path;
		result += kSavegamePath;
		Setting* localSavePath = GetINISetting("sLocalSavePath:General");
		if(localSavePath && (localSavePath->GetType() == Setting::kType_String))
			result += localSavePath->data.s;
		else
			result += "Saves\\";

		result += "\\";
		result += name;
		if (extension)
			result += extension;
		return result;
	}

	PluginCallbacks * GetPluginInfo(PluginHandle plugin)
	{
		if(plugin >= s_pluginCallbacks.size())
			s_pluginCallbacks.resize(plugin + 1);

		return &s_pluginCallbacks[plugin];
	}

	// plugin API
	void SetUniqueID(PluginHandle plugin, UInt32 uid)
	{
		// check existing plugins
		for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
		{
			if(iter->hadUID && (iter->uid == uid))
			{
				UInt32	collidingID = iter - s_pluginCallbacks.begin();

				_ERROR("plugin serialization UID collision (uid = %08X, plugins = %d %d)", plugin, uid, collidingID);
			}
		}

		PluginCallbacks * info = GetPluginInfo(plugin);

		ASSERT(!info->hadUID);

		info->uid = uid;
		info->hadUID = true;
	}

	void SetRevertCallback(PluginHandle plugin, F4SESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->revert = callback;
	}

	void SetSaveCallback(PluginHandle plugin, F4SESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->save = callback;
	}

	void SetLoadCallback(PluginHandle plugin, F4SESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->load = callback;
	}

	void SetFormDeleteCallback(PluginHandle plugin, F4SESerializationInterface::FormDeleteCallback callback)
	{
		GetPluginInfo(plugin)->formDelete = callback;
	}

	void SetSaveName(const char * name)
	{
		if(name)
		{
			_MESSAGE("save name is %s", name);
			s_savePath = MakeSavePath(name, ".f4se");
			_MESSAGE("full save path: %s", s_savePath.c_str());
		}
		else
		{
			_MESSAGE("cleared save path");
			s_savePath.clear();
		}
	}

	bool WriteRecord(UInt32 type, UInt32 version, const void * buf, UInt32 length)
	{
		if(!OpenRecord(type, version))
			return false;

		return WriteRecordData(buf, length);
	}

	// flush a chunk header to the file if one is currently open
	static void FlushWriteChunk(void)
	{
		if(!s_chunkOpen)
			return;

		UInt64	curOffset = s_currentFile.GetOffset();
		UInt64	chunkSize = curOffset - s_chunkHeaderOffset - sizeof(s_chunkHeader);

		ASSERT(chunkSize < 0x80000000);	// stupidity check

		s_chunkHeader.length = (UInt32)chunkSize;

		s_currentFile.SetOffset(s_chunkHeaderOffset);
		s_currentFile.WriteBuf(&s_chunkHeader, sizeof(s_chunkHeader));

		s_currentFile.SetOffset(curOffset);

		s_pluginHeader.length += chunkSize + sizeof(s_chunkHeader);

		s_chunkOpen = false;
	}

	bool OpenRecord(UInt32 type, UInt32 version)
	{
		if(!s_pluginHeader.numChunks)
		{
			ASSERT(!s_chunkOpen);

			s_pluginHeaderOffset = s_currentFile.GetOffset();
			s_currentFile.Skip(sizeof(s_pluginHeader));
		}

		FlushWriteChunk();

		s_chunkHeaderOffset = s_currentFile.GetOffset();
		s_currentFile.Skip(sizeof(s_chunkHeader));

		s_pluginHeader.numChunks++;

		s_chunkHeader.type = type;
		s_chunkHeader.version = version;
		s_chunkHeader.length = 0;

		s_chunkOpen = true;

		return true;
	}

	bool WriteRecordData(const void * buf, UInt32 length)
	{
		s_currentFile.WriteBuf(buf, length);

		return true;
	}

	static void FlushReadRecord(void)
	{
		if(s_chunkOpen)
		{
			if(s_chunkHeader.length)
			{
				// _WARNING("plugin didn't finish reading chunk");
				s_currentFile.Skip(s_chunkHeader.length);
			}

			s_chunkOpen = false;
		}
	}

	bool GetNextRecordInfo(UInt32 * type, UInt32 * version, UInt32 * length)
	{
		FlushReadRecord();

		if(!s_pluginHeader.numChunks)
			return false;

		s_pluginHeader.numChunks--;

		s_currentFile.ReadBuf(&s_chunkHeader, sizeof(s_chunkHeader));

		*type =		s_chunkHeader.type;
		*version =	s_chunkHeader.version;
		*length =	s_chunkHeader.length;

		s_chunkOpen = true;

		return true;
	}

	UInt32 ReadRecordData(void * buf, UInt32 length)
	{
		ASSERT(s_chunkOpen);

		if(length > s_chunkHeader.length)
			length = s_chunkHeader.length;

		s_currentFile.ReadBuf(buf, length);

		s_chunkHeader.length -= length;

		return length;
	}

	bool ResolveFormId(UInt32 formId, UInt32 * formIdOut)
	{
		UInt32	modID = formId >> 24;

		if (modID == 0xFF)
		{
			*formIdOut = formId;
			return true;
		}

		if (modID == 0xFE)
		{
			modID = formId >> 12;
		}

		UInt32	loadedModID = ResolveModIndex(modID);
		if (loadedModID < 0xFF)
		{
			*formIdOut = (formId & 0x00FFFFFF) | (((UInt32)loadedModID) << 24);
			return true;
		}
		else if (loadedModID > 0xFF)
		{
			*formIdOut = (loadedModID << 12) | (formId & 0x00000FFF);
			return true;
		}
		return false;
	}

	bool ResolveHandle(UInt64 handle, UInt64 * handleOut)
	{
		UInt32	modID = (handle & 0xFF000000) >> 24;

		if (modID == 0xFF)
		{
			*handleOut = handle;
			return true;
		}

		if (modID == 0xFE)
		{
			modID = (handle >> 12) & 0xFFFFF;
		}

		UInt64	loadedModID = (UInt64)ResolveModIndex(modID);
		if (loadedModID < 0xFF)
		{
			*handleOut = (handle & 0xFFFFFFFF00FFFFFF) | (((UInt64)loadedModID) << 24);
			return true;
		}
		else if (loadedModID > 0xFF)
		{
			*handleOut = (handle & 0xFFFFFFFF00000FFF) | (loadedModID << 12);
			return true;
		}
		return false;
	}

	// internal event handlers
	void HandleRevertGlobalData(void)
	{
		for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			if(s_pluginCallbacks[i].revert)
				s_pluginCallbacks[i].revert(&g_F4SESerializationInterface);
	}

	void HandleSaveGlobalData(void)
	{
		_MESSAGE("creating co-save");
		if(_access(s_savePath.c_str(), 0) == 0)
			DeleteFile(s_savePath.c_str());
		if(!s_currentFile.Create(s_savePath.c_str()))
		{
			_ERROR("HandleSaveGlobalData: couldn't create save file (%s)", s_savePath.c_str());
			return;
		}

		try
		{
			// init header
			s_fileHeader.signature =		Header::kSignature;
			s_fileHeader.formatVersion =	Header::kVersion;
			s_fileHeader.f4seVersion =		PACKED_F4SE_VERSION;
			s_fileHeader.runtimeVersion =	RUNTIME_VERSION;
			s_fileHeader.numPlugins =		0;

			s_currentFile.Skip(sizeof(s_fileHeader));

			// iterate through plugins
			for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			{
				PluginCallbacks	* info = &s_pluginCallbacks[i];

				if(info->save && info->hadUID)
				{
					// set up header info
					s_currentPlugin = i;

					s_pluginHeader.signature = info->uid;
					s_pluginHeader.numChunks = 0;
					s_pluginHeader.length = 0;

					s_chunkOpen = false;

					// call the plugin
					try
					{
						info->save(&g_F4SESerializationInterface);
					}
					catch( ... )
					{
						_ERROR("HandleSaveGlobalData: exception occurred saving %08X at %016I64X data may be corrupt.", s_pluginHeader.signature, s_currentFile.GetOffset());
					}

					// flush the remaining chunk data
					FlushWriteChunk();

					if(s_pluginHeader.numChunks)
					{
						UInt64	curOffset = s_currentFile.GetOffset();

						s_currentFile.SetOffset(s_pluginHeaderOffset);
						s_currentFile.WriteBuf(&s_pluginHeader, sizeof(s_pluginHeader));

						s_currentFile.SetOffset(curOffset);

						s_fileHeader.numPlugins++;
					}
				}
			}

			// write header
			s_currentFile.SetOffset(0);
			s_currentFile.WriteBuf(&s_fileHeader, sizeof(s_fileHeader));
		}
		catch(...)
		{
			_ERROR("HandleSaveGame: exception during save");
		}

		s_currentFile.Close();
	}

	void HandleLoadGlobalData(void)
	{
		_MESSAGE("loading co-save");

		if(!s_currentFile.Open(s_savePath.c_str()))
		{
			return;
		}

		try
		{
			Header	header;

			s_currentFile.ReadBuf(&header, sizeof(header));

			if(header.signature != Header::kSignature)
			{
				_ERROR("HandleLoadGame: invalid file signature (found %08X expected %08X)", header.signature, Header::kSignature);
				goto done;
			}

			if(header.formatVersion <= Header::kVersion_Invalid)
			{
				_ERROR("HandleLoadGame: version invalid (%08X)", header.formatVersion);
				goto done;
			}

			if(header.formatVersion > Header::kVersion)
			{
				_ERROR("HandleLoadGame: version too new (found %08X current %08X)", header.formatVersion, Header::kVersion);
				goto done;
			}

			// reset flags
			for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
				iter->hadData = false;
			
			// iterate through plugin data chunks
			while(s_currentFile.GetRemain() >= sizeof(PluginHeader))
			{
				s_currentFile.ReadBuf(&s_pluginHeader, sizeof(s_pluginHeader));

				UInt64	pluginChunkStart = s_currentFile.GetOffset();

				UInt32	pluginIdx = kPluginHandle_Invalid;

				for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
					if(iter->hadUID && (iter->uid == s_pluginHeader.signature))
						pluginIdx = iter - s_pluginCallbacks.begin();

				try
				{
					if(pluginIdx != kPluginHandle_Invalid)
					{
						PluginCallbacks	* info = &s_pluginCallbacks[pluginIdx];

						info->hadData = true;

						if(info->load)
						{
							s_chunkOpen = false;
							info->load(&g_F4SESerializationInterface);
						}
					}
					else
					{
						_WARNING("HandleLoadGame: plugin with signature %08X not loaded", s_pluginHeader.signature);
					}
				}
				catch( ... )
				{
					_ERROR("HandleLoadGame: exception occurred loading %08X", s_pluginHeader.signature);
				}

				// if plugin failed to read all its data or threw exception, jump to the next chunk
				UInt64	expectedOffset = pluginChunkStart + s_pluginHeader.length;
				if(s_currentFile.GetOffset() != expectedOffset)
				{
					_WARNING("HandleLoadGame: plugin did not read all of its data (at %016I64X expected %016I64X)", s_currentFile.GetOffset(), expectedOffset);
					s_currentFile.SetOffset(expectedOffset);
				}
			}

			// call load on plugins that had no data
			for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter) {
				if(!iter->hadData && iter->load) {
					iter->load(&g_F4SESerializationInterface);
				}
			}
		}
		catch(...)
		{
			_ERROR("HandleLoadGame: exception during load");

			// ### this could be handled better, individually catch around each plugin so one plugin can't mess things up for everyone else
		}

	done:
		s_currentFile.Close();
	}

	void HandleDeleteSave(std::string saveName)
	{
		std::string savePath = MakeSavePath(saveName, NULL);
		std::string coSavePath = savePath;
		savePath += ".fos";
		coSavePath += ".f4se";

		// Old save file really gone?
		IFileStream	saveFile;
		if (!saveFile.Open(savePath.c_str()))
		{
			_MESSAGE("deleting co-save %s", coSavePath.c_str());	
			DeleteFile(coSavePath.c_str());
		}
		else
		{
			_MESSAGE("skipped delete of co-save %s", coSavePath.c_str());	
		}
	}

	void HandleDeletedForm(UInt64 handle)
	{
		for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			if(s_pluginCallbacks[i].formDelete)
				s_pluginCallbacks[i].formDelete(handle);
	}

	template <>
	bool WriteData<BSFixedString>(const F4SESerializationInterface * intfc, const BSFixedString * str)
	{
		return WriteData<const char>(intfc, str->c_str());
	}

	template <>
	bool ReadData<BSFixedString>(const F4SESerializationInterface * intfc, BSFixedString * str)
	{
		UInt16 len = 0;

		if (! intfc->ReadRecordData(&len, sizeof(len)))
			return false;
		if(len == 0)
			return true;
		if (len > SHRT_MAX)
			return false;

		char * buf = new char[len + 1];
		buf[0] = 0;

		if (! intfc->ReadRecordData(buf, len)) {
			delete [] buf;
			return false;
		}
		buf[len] = 0;

		*str = BSFixedString(buf);
		delete [] buf;
		return true;
	}

	template <>
	bool WriteData<std::string>(const F4SESerializationInterface * intfc, const std::string * str)
	{
		UInt16 len = str->length();
		if (len > SHRT_MAX)
			return false;
		if (! intfc->WriteRecordData(&len, sizeof(len)))
			return false;
		if (len == 0)
			return true;
		if (! intfc->WriteRecordData(str->data(), len))
			return false;
		return true;
	}

	template <>
	bool ReadData<std::string>(const F4SESerializationInterface * intfc, std::string * str)
	{
		UInt16 len = 0;
		if (! intfc->ReadRecordData(&len, sizeof(len)))
			return false;
		if (len == 0)
			return true;
		if (len > SHRT_MAX)
			return false;

		char * buf = new char[len + 1];
		buf[0] = 0;

		if (! intfc->ReadRecordData(buf, len)) {
			delete [] buf;
			return false;
		}
		buf[len] = 0;

		*str = std::string(buf);
		delete [] buf;
		return true;
	}

	template <>
	bool WriteData<const char>(const F4SESerializationInterface * intfc, const char* str)
	{
		UInt16 len = strlen(str);
		if (len > SHRT_MAX)
			return false;
		if (! intfc->WriteRecordData(&len, sizeof(len)))
			return false;
		if (len == 0)
			return true;
		if (! intfc->WriteRecordData(str, len))
			return false;
		return true;
	}
}
