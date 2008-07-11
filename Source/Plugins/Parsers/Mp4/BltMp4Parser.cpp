/*****************************************************************
|
|   MP4 Parser Module
|
|   (c) 2002-2006 Gilles Boccon-Gibod
|   Author: Gilles Boccon-Gibod (bok@bok.net)
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4.h"
#include "Ap4AtomixAdapters.h"
#include "Atomix.h"
#include "BltConfig.h"
#include "BltMp4Parser.h"
#include "BltCore.h"
#include "BltMediaNode.h"
#include "BltMedia.h"
#include "BltPcm.h"
#include "BltPacketProducer.h"
#include "BltByteStreamUser.h"
#include "BltStream.h"
#include "BltCommonMediaTypes.h"

/*----------------------------------------------------------------------
|   logging
+---------------------------------------------------------------------*/
ATX_SET_LOCAL_LOGGER("bluetune.plugins.parsers.mp4")

/*----------------------------------------------------------------------
|   types
+---------------------------------------------------------------------*/
struct Mp4ParserModule {
    /* base class */
    ATX_EXTENDS(BLT_BaseModule);

    /* members */
    BLT_UInt32 mp4_audio_type_id;
    BLT_UInt32 mp4_video_type_id;
    BLT_UInt32 mp4_es_type_id;
    BLT_UInt32 iso_base_es_type_id;
};

// it is important to keep this structure a POD (no methods)
// because the strict compilers will not like use using
// the offsetof() macro necessary when using ATX_SELF()
typedef struct {
    /* interfaces */
    ATX_IMPLEMENTS(BLT_MediaPort);
    ATX_IMPLEMENTS(BLT_InputStreamUser);

    /* members */
    BLT_MediaType audio_media_type;
    BLT_MediaType video_media_type;
    AP4_File*     mp4_file;
    AP4_Track*    mp4_track;
} Mp4ParserInput;

// it is important to keep this structure a POD (no methods)
// because the strict compilers will not like use using
// the offsetof() macro necessary when using ATX_SELF()
typedef struct {
    /* interfaces */
    ATX_IMPLEMENTS(BLT_MediaPort);
    ATX_IMPLEMENTS(BLT_PacketProducer);

    /* members */
    BLT_UInt32      mp4_es_type_id;
    BLT_UInt32      iso_base_es_type_id;
    BLT_MediaType*  media_type;
    BLT_Ordinal     sample;
    AP4_DataBuffer* sample_buffer;
} Mp4ParserOutput;

// it is important to keep this structure a POD (no methods)
// because the strict compilers will not like use using
// the offsetof() macro necessary when using ATX_SELF()
typedef struct {
    /* base class */
    ATX_EXTENDS(BLT_BaseMediaNode);

    /* members */
    Mp4ParserInput  input;
    Mp4ParserOutput output;
} Mp4Parser;

/*----------------------------------------------------------------------
|   Mp4ParserInput_Construct
+---------------------------------------------------------------------*/
static void
Mp4ParserInput_Construct(Mp4ParserInput* self, BLT_Module* module)
{
    Mp4ParserModule* mp4_parser_module = (Mp4ParserModule*)module;
    BLT_MediaType_Init(&self->audio_media_type, mp4_parser_module->mp4_audio_type_id);
    BLT_MediaType_Init(&self->video_media_type, mp4_parser_module->mp4_video_type_id);
    self->mp4_file = NULL;
    self->mp4_track = NULL;
}

/*----------------------------------------------------------------------
|   Mp4ParserInput_Destruct
+---------------------------------------------------------------------*/
static void
Mp4ParserInput_Destruct(Mp4ParserInput* self)
{
    delete self->mp4_file;
}

/*----------------------------------------------------------------------
|   Mp4ParserInput_SetStream
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserInput_SetStream(BLT_InputStreamUser* _self,
                         ATX_InputStream*     stream,
                         const BLT_MediaType* stream_media_type)
{
    Mp4Parser* self = ATX_SELF_M(input, Mp4Parser, BLT_InputStreamUser);

    /* check media type */
    if (stream_media_type == NULL || 
        (stream_media_type->id != self->input.audio_media_type.id &&
         stream_media_type->id != self->input.video_media_type.id)) {
        return BLT_ERROR_INVALID_MEDIA_FORMAT;
    }

    /* if we had a file before, release it now */
    delete self->input.mp4_file;

    /* parse the MP4 file */
    ATX_LOG_FINE("Mp4ParserInput::SetStream - parsing MP4 file");
    AP4_ByteStream* stream_adapter = new ATX_InputStream_To_AP4_ByteStream_Adapter(stream);
    self->input.mp4_file = new AP4_File(*stream_adapter, 
                                        AP4_DefaultAtomFactory::Instance,
                                        true); /* parse until moov only */
    stream_adapter->Release();

    /* declare variables here to avoid GCC warnings */
    AP4_SampleDescription*      sample_desc = NULL;
    AP4_MpegSampleDescription*  mpeg_desc = NULL;
    AP4_AudioSampleDescription* audio_desc = NULL;
    BLT_StreamInfo              stream_info;

    /* find the audio track */
    AP4_Movie* movie = self->input.mp4_file->GetMovie();
    if (movie == NULL) {
        ATX_LOG_FINE("Mp4ParserInput::SetStream - no movie in file");
        goto fail;
    }
    self->input.mp4_track = movie->GetTrack(AP4_Track::TYPE_AUDIO);
    if (self->input.mp4_track == NULL) {
        ATX_LOG_FINE("Mp4ParserInput::SetStream - no audio track found");
        goto fail;
    }

    // check that the track is of the right type
    sample_desc = self->input.mp4_track->GetSampleDescription(0);
    if (sample_desc == NULL) {
        ATX_LOG_FINE("Mp4ParserInput::SetStream - no sample description");
        goto fail;
    }
    audio_desc = dynamic_cast<AP4_AudioSampleDescription*>(sample_desc);
    if (audio_desc == NULL) {
        ATX_LOG_FINE("Mp4ParserInput::SetStream - sample description is not audio");
        goto fail;
    }

    // update the stream info
    stream_info.duration        = self->input.mp4_track->GetDurationMs();
    stream_info.channel_count   = audio_desc->GetChannelCount();
    stream_info.sample_rate     = audio_desc->GetSampleRate();
    stream_info.mask = BLT_STREAM_INFO_MASK_DURATION        |
                       BLT_STREAM_INFO_MASK_CHANNEL_COUNT   |
                       BLT_STREAM_INFO_MASK_SAMPLE_RATE;
    
    if (sample_desc->GetType() == AP4_SampleDescription::TYPE_MPEG) {
        ATX_LOG_FINE("Mp4ParserInput::SetStream - sample description is of type MPEG");
        mpeg_desc = dynamic_cast<AP4_MpegSampleDescription*>(sample_desc);
    }
    if (mpeg_desc) {
        stream_info.data_type       = mpeg_desc->GetObjectTypeString(mpeg_desc->GetObjectTypeId());
        stream_info.average_bitrate = mpeg_desc->GetAvgBitrate();
        stream_info.nominal_bitrate = mpeg_desc->GetAvgBitrate();
        stream_info.mask |= BLT_STREAM_INFO_MASK_AVERAGE_BITRATE |
                            BLT_STREAM_INFO_MASK_NOMINAL_BITRATE |
                            BLT_STREAM_INFO_MASK_DATA_TYPE;
    }
    BLT_Stream_SetInfo(ATX_BASE(self, BLT_BaseMediaNode).context, &stream_info);

    // setup the output media type
    if (mpeg_desc) {
        unsigned int decoder_info_length = mpeg_desc->GetDecoderInfo().GetDataSize();
        BLT_Mpeg4AudioMediaType* media_type = (BLT_Mpeg4AudioMediaType*)ATX_AllocateZeroMemory(sizeof(BLT_Mpeg4AudioMediaType)+decoder_info_length-1);
        BLT_MediaType_Init(&media_type->base, self->output.mp4_es_type_id);
        media_type->base.extension_size = sizeof(BLT_Mpeg4AudioMediaType)+decoder_info_length-1-sizeof(BLT_MediaType);
        media_type->object_type_id      = mpeg_desc->GetObjectTypeId();
        media_type->decoder_info_length =  decoder_info_length;
        if (decoder_info_length) ATX_CopyMemory(&media_type->decoder_info[0], mpeg_desc->GetDecoderInfo().GetData(), decoder_info_length);
        self->output.media_type = (BLT_MediaType*)media_type;
    } else {
        // here we have to be format-specific for the decoder info
        AP4_MemoryByteStream* mbs = NULL;
        if (sample_desc->GetFormat() == AP4_SAMPLE_FORMAT_ALAC) {
            // look for an 'alac' atom (either top-level or inside a 'wave') 
            AP4_Atom* alac = sample_desc->GetDetails().GetChild(AP4_SAMPLE_FORMAT_ALAC);
            if (alac == NULL) {
                AP4_ContainerAtom* wave = dynamic_cast<AP4_ContainerAtom*>(sample_desc->GetDetails().GetChild(AP4_ATOM_TYPE_WAVE));
                if (wave) {
                    alac = wave->GetChild(AP4_SAMPLE_FORMAT_ALAC);
                }
            }
            if (alac) {
                // pass the alac payload as the decoder info
                mbs = new AP4_MemoryByteStream((AP4_Size)alac->GetSize());
                alac->WriteFields(*mbs);
            } 
        }
        AP4_LargeSize decoder_info_length = mbs?mbs->GetDataSize():0;
        BLT_IsoBaseAudioMediaType* media_type = (BLT_IsoBaseAudioMediaType*)ATX_AllocateZeroMemory(sizeof(BLT_IsoBaseAudioMediaType)+decoder_info_length-1);
        BLT_MediaType_Init(&media_type->base, self->output.iso_base_es_type_id);
        media_type->format = sample_desc->GetFormat();
        media_type->base.extension_size = sizeof(BLT_IsoBaseAudioMediaType)+decoder_info_length-1-sizeof(BLT_MediaType);
        media_type->decoder_info_length =  decoder_info_length;
        if (mbs) ATX_CopyMemory(&media_type->decoder_info[0], mbs->GetData(), mbs->GetDataSize());
        self->output.media_type = (BLT_MediaType*)media_type;
        
        if (mbs) mbs->Release();
    }
    
    return BLT_SUCCESS;

fail:
    delete self->input.mp4_file;
    self->input.mp4_file = NULL;
    return BLT_ERROR_INVALID_MEDIA_FORMAT;
}

/*----------------------------------------------------------------------
|   Mp4ParserInput_QueryMediaType
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserInput_QueryMediaType(BLT_MediaPort*        _self,
                              BLT_Ordinal           index,
                              const BLT_MediaType** media_type)
{
    Mp4ParserInput* self = ATX_SELF(Mp4ParserInput, BLT_MediaPort);
    
    if (index == 0) {
        *media_type = &self->audio_media_type;
        return BLT_SUCCESS;
    } else if (index == 0) {
        *media_type = &self->video_media_type;
        return BLT_SUCCESS;
    } else {
        *media_type = NULL;
        return BLT_FAILURE;
    }
}

/*----------------------------------------------------------------------
|   GetInterface implementation
+---------------------------------------------------------------------*/
ATX_BEGIN_GET_INTERFACE_IMPLEMENTATION(Mp4ParserInput)
    ATX_GET_INTERFACE_ACCEPT(Mp4ParserInput, BLT_MediaPort)
    ATX_GET_INTERFACE_ACCEPT(Mp4ParserInput, BLT_InputStreamUser)
ATX_END_GET_INTERFACE_IMPLEMENTATION

/*----------------------------------------------------------------------
|   BLT_InputStreamUser interface
+---------------------------------------------------------------------*/
ATX_BEGIN_INTERFACE_MAP(Mp4ParserInput, BLT_InputStreamUser)
    Mp4ParserInput_SetStream
ATX_END_INTERFACE_MAP

/*----------------------------------------------------------------------
|   BLT_MediaPort interface
+---------------------------------------------------------------------*/
BLT_MEDIA_PORT_IMPLEMENT_SIMPLE_TEMPLATE(Mp4ParserInput, 
                                         "input",
                                         STREAM_PULL,
                                         IN)
ATX_BEGIN_INTERFACE_MAP(Mp4ParserInput, BLT_MediaPort)
    Mp4ParserInput_GetName,
    Mp4ParserInput_GetProtocol,
    Mp4ParserInput_GetDirection,
    Mp4ParserInput_QueryMediaType
ATX_END_INTERFACE_MAP

/*----------------------------------------------------------------------
|   Mp4ParserOutput_Construct
+---------------------------------------------------------------------*/
static void
Mp4ParserOutput_Construct(Mp4ParserOutput* self)
{
    self->media_type    = NULL;
    self->sample        = 0;
    self->sample_buffer = new AP4_DataBuffer();
}

/*----------------------------------------------------------------------
|   Mp4ParserOutput_Destruct
+---------------------------------------------------------------------*/
static void
Mp4ParserOutput_Destruct(Mp4ParserOutput* self)
{
    /* release the sample buffer */
    delete self->sample_buffer;

    /* free the media type extensions */
    BLT_MediaType_Free((BLT_MediaType*)self->media_type);
}

/*----------------------------------------------------------------------
|   Mp4ParserOutput_QueryMediaType
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserOutput_QueryMediaType(BLT_MediaPort*        _self,
                               BLT_Ordinal           index,
                               const BLT_MediaType** media_type)
{
    Mp4ParserOutput* self = ATX_SELF(Mp4ParserOutput, BLT_MediaPort);
    
    if (index == 0) {
        *media_type = (BLT_MediaType*)self->media_type;
        return BLT_SUCCESS;
    } else {
        *media_type = NULL;
        return BLT_FAILURE;
    }
}

/*----------------------------------------------------------------------
|   Mp4ParserOutput_GetPacket
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserOutput_GetPacket(BLT_PacketProducer* _self,
                          BLT_MediaPacket**   packet)
{
    Mp4Parser* self = ATX_SELF_M(output, Mp4Parser, BLT_PacketProducer);

    *packet = NULL;
    if (self->input.mp4_track == NULL) {
        return BLT_ERROR_PORT_HAS_NO_DATA;
    } else {
        // check for end-of-stream
        if (self->output.sample >= self->input.mp4_track->GetSampleCount()) {
            return BLT_ERROR_EOS;
        }

        // read one sample
        AP4_Sample sample;
        AP4_Result result = self->input.mp4_track->ReadSample(self->output.sample++, sample, *self->output.sample_buffer);
        if (AP4_FAILED(result)) {
            ATX_LOG_WARNING_1("Mp4ParserOutput::GetPacket - ReadSample failed (%d)", result);
            return BLT_ERROR_PORT_HAS_NO_DATA;
        }

        AP4_Size packet_size = self->output.sample_buffer->GetDataSize();
        result = BLT_Core_CreateMediaPacket(ATX_BASE(self, BLT_BaseMediaNode).core,
                                            packet_size,
                                            (BLT_MediaType*)self->output.media_type,
                                            packet);
        if (BLT_FAILED(result)) return result;
        BLT_MediaPacket_SetPayloadSize(*packet, packet_size);
        void* buffer = BLT_MediaPacket_GetPayloadBuffer(*packet);
        ATX_CopyMemory(buffer, self->output.sample_buffer->GetData(), packet_size);

        // set the timestamp
        AP4_UI32 media_timescale = self->input.mp4_track->GetMediaTimeScale();
        if (media_timescale) {
            AP4_UI64 ts = ((AP4_UI64)sample.GetCts())*1000000;
            ts /= media_timescale;
            BLT_TimeStamp bt_ts = {
                (BLT_Int32)(ts / 1000000),
                (BLT_Int32)((ts % 1000000)*1000)
            };
            BLT_MediaPacket_SetTimeStamp(*packet, bt_ts);
        }

        // set packet flags
        if (self->output.sample == 0) {
            BLT_MediaPacket_SetFlags(*packet, BLT_MEDIA_PACKET_FLAG_START_OF_STREAM);
        }

        return BLT_SUCCESS;
    }
}

/*----------------------------------------------------------------------
|   GetInterface implementation
+---------------------------------------------------------------------*/
ATX_BEGIN_GET_INTERFACE_IMPLEMENTATION(Mp4ParserOutput)
    ATX_GET_INTERFACE_ACCEPT(Mp4ParserOutput, BLT_MediaPort)
    ATX_GET_INTERFACE_ACCEPT(Mp4ParserOutput, BLT_PacketProducer)
ATX_END_GET_INTERFACE_IMPLEMENTATION

/*----------------------------------------------------------------------
|   BLT_MediaPort interface
+---------------------------------------------------------------------*/
BLT_MEDIA_PORT_IMPLEMENT_SIMPLE_TEMPLATE(Mp4ParserOutput, 
                                         "output",
                                         PACKET,
                                         OUT)
ATX_BEGIN_INTERFACE_MAP(Mp4ParserOutput, BLT_MediaPort)
    Mp4ParserOutput_GetName,
    Mp4ParserOutput_GetProtocol,
    Mp4ParserOutput_GetDirection,
    Mp4ParserOutput_QueryMediaType
ATX_END_INTERFACE_MAP

/*----------------------------------------------------------------------
|   BLT_InputStreamProvider interface
+---------------------------------------------------------------------*/
ATX_BEGIN_INTERFACE_MAP(Mp4ParserOutput, BLT_PacketProducer)
    Mp4ParserOutput_GetPacket
ATX_END_INTERFACE_MAP

/*----------------------------------------------------------------------
|   Mp4Parser_Destroy
+---------------------------------------------------------------------*/
static BLT_Result
Mp4Parser_Destroy(Mp4Parser* self)
{
    ATX_LOG_FINE("Mp4Parser::Destroy");

    /* destruct the members */
    Mp4ParserInput_Destruct(&self->input);
    Mp4ParserOutput_Destruct(&self->output);

    /* destruct the inherited object */
    BLT_BaseMediaNode_Destruct(&ATX_BASE(self, BLT_BaseMediaNode));

    delete self;

    return BLT_SUCCESS;
}

/*----------------------------------------------------------------------
|    Mp4Parser_Deactivate
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4Parser_Deactivate(BLT_MediaNode* _self)
{
    ATX_LOG_FINER("Mp4Parser::Deactivate");

    /* call the base class method */
    BLT_BaseMediaNode_Deactivate(_self);

    return BLT_SUCCESS;
}

/*----------------------------------------------------------------------
|   Mp4Parser_GetPortByName
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4Parser_GetPortByName(BLT_MediaNode*  _self,
                         BLT_CString     name,
                         BLT_MediaPort** port)
{
    Mp4Parser* self = ATX_SELF_EX(Mp4Parser, BLT_BaseMediaNode, BLT_MediaNode);

    if (ATX_StringsEqual(name, "input")) {
        *port = &ATX_BASE(&self->input, BLT_MediaPort);
        return BLT_SUCCESS;
    } else if (ATX_StringsEqual(name, "output")) {
        *port = &ATX_BASE(&self->output, BLT_MediaPort);
        return BLT_SUCCESS;
    } else {
        *port = NULL;
        return BLT_ERROR_NO_SUCH_PORT;
    }
}

/*----------------------------------------------------------------------
|   Mp4Parser_Seek
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4Parser_Seek(BLT_MediaNode* _self,
               BLT_SeekMode*  mode,
               BLT_SeekPoint* point)
{
    Mp4Parser* self = ATX_SELF_EX(Mp4Parser, BLT_BaseMediaNode, BLT_MediaNode);

    /* estimate the seek point */
    if (ATX_BASE(self, BLT_BaseMediaNode).context == NULL) return BLT_FAILURE;
    BLT_Stream_EstimateSeekPoint(ATX_BASE(self, BLT_BaseMediaNode).context, *mode, point);
    if (!(point->mask & BLT_SEEK_POINT_MASK_TIME_STAMP)) {
        return BLT_FAILURE;
    }

    /* seek to the estimated offset */
    AP4_Ordinal   sample_index = 0;
    AP4_TimeStamp ts_ms = point->time_stamp.seconds*1000+point->time_stamp.nanoseconds/1000000;
    AP4_Result result = self->input.mp4_track->GetSampleIndexForTimeStampMs(ts_ms, sample_index);
    if (AP4_FAILED(result)) {
        ATX_LOG_WARNING_1("Mp4Parser::Seek - GetSampleIndexForTimeStampMs failed (%d)", result);
        return BLT_FAILURE;
    }
    self->output.sample = sample_index;

    /* set the mode so that the nodes down the chaine know the seek has */
    /* already been done on the stream                                  */
    *mode = BLT_SEEK_MODE_IGNORE;

    return BLT_SUCCESS;
}

/*----------------------------------------------------------------------
|    GetInterface implementation
+---------------------------------------------------------------------*/
ATX_BEGIN_GET_INTERFACE_IMPLEMENTATION(Mp4Parser)
    ATX_GET_INTERFACE_ACCEPT_EX(Mp4Parser, BLT_BaseMediaNode, BLT_MediaNode)
    ATX_GET_INTERFACE_ACCEPT_EX(Mp4Parser, BLT_BaseMediaNode, ATX_Referenceable)
ATX_END_GET_INTERFACE_IMPLEMENTATION

/*----------------------------------------------------------------------
|    BLT_MediaNode interface
+---------------------------------------------------------------------*/
ATX_BEGIN_INTERFACE_MAP_EX(Mp4Parser, BLT_BaseMediaNode, BLT_MediaNode)
    BLT_BaseMediaNode_GetInfo,
    Mp4Parser_GetPortByName,
    BLT_BaseMediaNode_Activate,
    Mp4Parser_Deactivate,
    BLT_BaseMediaNode_Start,
    BLT_BaseMediaNode_Stop,
    BLT_BaseMediaNode_Pause,
    BLT_BaseMediaNode_Resume,
    Mp4Parser_Seek
ATX_END_INTERFACE_MAP_EX

/*----------------------------------------------------------------------
|   ATX_Referenceable interface
+---------------------------------------------------------------------*/
ATX_IMPLEMENT_REFERENCEABLE_INTERFACE_EX(Mp4Parser, 
                                         BLT_BaseMediaNode, 
                                         reference_count)

/*----------------------------------------------------------------------
|   Mp4Parser_Construct
+---------------------------------------------------------------------*/
static void
Mp4Parser_Construct(Mp4Parser* self, BLT_Module* module, BLT_Core* core)
{
    /* construct the inherited object */
    BLT_BaseMediaNode_Construct(&ATX_BASE(self, BLT_BaseMediaNode), module, core);

    /* construct the members */
    Mp4ParserInput_Construct(&self->input, module);
    Mp4ParserOutput_Construct(&self->output);

    /* setup media types */
    Mp4ParserModule* mp4_parser_module = (Mp4ParserModule*)module;
    self->output.mp4_es_type_id = mp4_parser_module->mp4_es_type_id;
    self->output.iso_base_es_type_id = mp4_parser_module->iso_base_es_type_id;

    /* setup interfaces */
    ATX_SET_INTERFACE_EX(self, Mp4Parser, BLT_BaseMediaNode, BLT_MediaNode);
    ATX_SET_INTERFACE_EX(self, Mp4Parser, BLT_BaseMediaNode, ATX_Referenceable);
    ATX_SET_INTERFACE(&self->input,  Mp4ParserInput,  BLT_MediaPort);
    ATX_SET_INTERFACE(&self->input,  Mp4ParserInput,  BLT_InputStreamUser);
    ATX_SET_INTERFACE(&self->output, Mp4ParserOutput, BLT_MediaPort);
    ATX_SET_INTERFACE(&self->output, Mp4ParserOutput, BLT_PacketProducer);
}

/*----------------------------------------------------------------------
|   Mp4Parser_Create
+---------------------------------------------------------------------*/
static BLT_Result
Mp4Parser_Create(BLT_Module*              module,
                 BLT_Core*                core, 
                 BLT_ModuleParametersType parameters_type,
                 BLT_AnyConst             parameters, 
                 BLT_MediaNode**          object)
{
    Mp4Parser* self;

    ATX_LOG_FINE("Mp4Parser::Create");

    /* check parameters */
    if (parameters == NULL || 
        parameters_type != BLT_MODULE_PARAMETERS_TYPE_MEDIA_NODE_CONSTRUCTOR) {
        return BLT_ERROR_INVALID_PARAMETERS;
    }

    /* allocate the object */
    self = new Mp4Parser();
    Mp4Parser_Construct(self, module, core);

    /* return the object */
    *object = &ATX_BASE_EX(self, BLT_BaseMediaNode, BLT_MediaNode);

    return BLT_SUCCESS;
}

/*----------------------------------------------------------------------
|   Mp4ParserModule_Attach
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserModule_Attach(BLT_Module* _self, BLT_Core* core)
{
    Mp4ParserModule* self = ATX_SELF_EX(Mp4ParserModule, BLT_BaseModule, BLT_Module);
    BLT_Registry*    registry;
    BLT_Result       result;

    /* get the registry */
    result = BLT_Core_GetRegistry(core, &registry);
    if (BLT_FAILED(result)) return result;

    /* register the ".mp4" file extension */
    result = BLT_Registry_RegisterExtension(registry, 
                                            ".mp4",
                                            "audio/mp4");
    if (BLT_FAILED(result)) return result;

    /* register the ".m4a" file extension */
    result = BLT_Registry_RegisterExtension(registry, 
                                            ".m4a",
                                            "audio/mp4");
    if (BLT_FAILED(result)) return result;

    /* register the ".m4v" file extension */
    result = BLT_Registry_RegisterExtension(registry, 
                                            ".m4v",
                                            "video/mp4");
    if (BLT_FAILED(result)) return result;

    /* register the ".3gp" file extension */
    result = BLT_Registry_RegisterExtension(registry, 
                                            ".3gp",
                                            "audio/mp4");
    if (BLT_FAILED(result)) return result;

    /* register the ".3gp" file extension */
    result = BLT_Registry_RegisterExtension(registry, 
                                            ".mov",
                                            "audio/mp4");
    if (BLT_FAILED(result)) return result;

    /* get the type id for "audio/mp4" */
    result = BLT_Registry_GetIdForName(
        registry,
        BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
        "audio/mp4",
        &self->mp4_audio_type_id);
    if (BLT_FAILED(result)) return result;
    ATX_LOG_FINE_1("MP4 Parser Module::Attach (audio/mp4 type = %d)", self->mp4_audio_type_id);
    
    /* get the type id for "video/mp4" */
    result = BLT_Registry_GetIdForName(
        registry,
        BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
        "video/mp4",
        &self->mp4_video_type_id);
    if (BLT_FAILED(result)) return result;
    ATX_LOG_FINE_1("MP4 Parser Module::Attach (video/mp4 type = %d)", self->mp4_video_type_id);

    /* register the type id for BLT_MP4_ES_MIME_TYPE */
    result = BLT_Registry_RegisterName(
        registry,
        BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
        BLT_MP4_ES_MIME_TYPE,
        &self->mp4_es_type_id);
    if (BLT_FAILED(result)) return result;
    ATX_LOG_FINE_1("MP4 Parser Module::Attach (" BLT_MP4_ES_MIME_TYPE " type = %d)", self->mp4_es_type_id);

    /* register the type id for BLT_ISO_BASE_ES_MIME_TYPE */
    result = BLT_Registry_RegisterName(
        registry,
        BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
        BLT_ISO_BASE_ES_MIME_TYPE,
        &self->iso_base_es_type_id);
    if (BLT_FAILED(result)) return result;
    ATX_LOG_FINE_1("MP4 Parser Module::Attach (" BLT_ISO_BASE_ES_MIME_TYPE " type = %d)", self->iso_base_es_type_id);

    /* register mime type aliases */
    BLT_Registry_RegisterNameForId(registry, 
                                   BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
                                   "audio/m4a", self->mp4_audio_type_id);
    BLT_Registry_RegisterNameForId(registry, 
                                   BLT_REGISTRY_NAME_CATEGORY_MEDIA_TYPE_IDS,
                                   "video/m4v", self->mp4_video_type_id);

    return BLT_SUCCESS;
}

/*----------------------------------------------------------------------
|   Mp4ParserModule_Probe
+---------------------------------------------------------------------*/
BLT_METHOD
Mp4ParserModule_Probe(BLT_Module*              _self, 
                      BLT_Core*                core,
                      BLT_ModuleParametersType parameters_type,
                      BLT_AnyConst             parameters,
                      BLT_Cardinal*            match)
{
    Mp4ParserModule* self = ATX_SELF_EX(Mp4ParserModule, BLT_BaseModule, BLT_Module);
    BLT_COMPILER_UNUSED(core);

    switch (parameters_type) {
      case BLT_MODULE_PARAMETERS_TYPE_MEDIA_NODE_CONSTRUCTOR:
        {
            BLT_MediaNodeConstructor* constructor = 
                (BLT_MediaNodeConstructor*)parameters;

            /* we need the input protocol to be STREAM_PULL and the output */
            /* protocol to be PACKET                                       */
             if ((constructor->spec.input.protocol  != BLT_MEDIA_PORT_PROTOCOL_ANY &&
                  constructor->spec.input.protocol  != BLT_MEDIA_PORT_PROTOCOL_STREAM_PULL) ||
                 (constructor->spec.output.protocol != BLT_MEDIA_PORT_PROTOCOL_ANY &&
                  constructor->spec.output.protocol != BLT_MEDIA_PORT_PROTOCOL_PACKET)) {
                return BLT_FAILURE;
            }

            /* we need the input media type to be 'audio/mp4' or 'video/mp4' */
            if (constructor->spec.input.media_type->id != self->mp4_audio_type_id &&
                constructor->spec.input.media_type->id != self->mp4_video_type_id) {
                return BLT_FAILURE;
            }

            /* the output type should be unknown at this point */
            if (constructor->spec.output.media_type->id != 
                BLT_MEDIA_TYPE_ID_UNKNOWN) {
                return BLT_FAILURE;
            }

            /* compute the match level */
            if (constructor->name != NULL) {
                /* we're being probed by name */
                if (ATX_StringsEqual(constructor->name, "Mp4Parser")) {
                    /* our name */
                    *match = BLT_MODULE_PROBE_MATCH_EXACT;
                } else {
                    /* not out name */
                    return BLT_FAILURE;
                }
            } else {
                /* we're probed by protocol/type specs only */
                *match = BLT_MODULE_PROBE_MATCH_MAX - 10;
            }

            ATX_LOG_FINE_1("Mp4ParserModule::Probe - Ok [%d]", *match);
            return BLT_SUCCESS;
        }    
        break;

      default:
        break;
    }

    return BLT_FAILURE;
}

/*----------------------------------------------------------------------
|   GetInterface implementation
+---------------------------------------------------------------------*/
ATX_BEGIN_GET_INTERFACE_IMPLEMENTATION(Mp4ParserModule)
    ATX_GET_INTERFACE_ACCEPT_EX(Mp4ParserModule, BLT_BaseModule, BLT_Module)
    ATX_GET_INTERFACE_ACCEPT_EX(Mp4ParserModule, BLT_BaseModule, ATX_Referenceable)
ATX_END_GET_INTERFACE_IMPLEMENTATION

/*----------------------------------------------------------------------
|   node factory
+---------------------------------------------------------------------*/
BLT_MODULE_IMPLEMENT_SIMPLE_MEDIA_NODE_FACTORY(Mp4ParserModule, Mp4Parser)

/*----------------------------------------------------------------------
|   BLT_Module interface
+---------------------------------------------------------------------*/
ATX_BEGIN_INTERFACE_MAP_EX(Mp4ParserModule, BLT_BaseModule, BLT_Module)
    BLT_BaseModule_GetInfo,
    Mp4ParserModule_Attach,
    Mp4ParserModule_CreateInstance,
    Mp4ParserModule_Probe
ATX_END_INTERFACE_MAP

/*----------------------------------------------------------------------
|   ATX_Referenceable interface
+---------------------------------------------------------------------*/
#define Mp4ParserModule_Destroy(x) \
    BLT_BaseModule_Destroy((BLT_BaseModule*)(x))

ATX_IMPLEMENT_REFERENCEABLE_INTERFACE_EX(Mp4ParserModule, 
                                         BLT_BaseModule,
                                         reference_count)

/*----------------------------------------------------------------------
|   node constructor
+---------------------------------------------------------------------*/
BLT_MODULE_IMPLEMENT_SIMPLE_CONSTRUCTOR(Mp4ParserModule, "MP4 Parser", 0)

/*----------------------------------------------------------------------
|   module object
+---------------------------------------------------------------------*/
BLT_Result 
BLT_Mp4ParserModule_GetModuleObject(BLT_Module** object)
{
    if (object == NULL) return BLT_ERROR_INVALID_PARAMETERS;

    return Mp4ParserModule_Create(object);
}
