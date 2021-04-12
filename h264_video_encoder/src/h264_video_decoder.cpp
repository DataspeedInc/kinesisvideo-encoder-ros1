/*
 *  Copyright 2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws_ros1_common/sdk_utils/logging/aws_ros_logger.h>
#include <aws_ros1_common/sdk_utils/ros1_node_parameter_reader.h>
#include <h264_encoder_core/h264_decoder.h>
#include <h264_encoder_core/h264_decoder_node_config.h>
#include <image_transport/image_transport.h>
#include <kinesis_video_msgs/KinesisImageMetadata.h>
#include <kinesis_video_msgs/KinesisVideoFrame.h>
#include <ros/ros.h>
#include <sensor_msgs/image_encodings.h>

#include <map>
#include <string>
#include <vector>

using namespace Aws::Utils::Decoding;
using namespace Aws::Utils::Logging;

namespace Aws {
namespace Kinesis {

const std::map<std::string, AVPixelFormat> SNSR_IMG_ENC_to_LIBAV_PIXEL_FRMT = {
  {sensor_msgs::image_encodings::RGB8, AV_PIX_FMT_RGB24},
  {sensor_msgs::image_encodings::BGR8, AV_PIX_FMT_BGR24},
  {sensor_msgs::image_encodings::RGBA8, AV_PIX_FMT_RGBA},
  {sensor_msgs::image_encodings::BGRA8, AV_PIX_FMT_BGRA},
  {sensor_msgs::image_encodings::MONO8, AV_PIX_FMT_GRAY8}};

/**
 * Initialize the H264Encoder
 * @param msg the message from the image sensor through image transport
 * @param decoder reference to pointer that owns the H264Decoder instance. if
 *  the pointer is null, it will be modified to the address of the new H264Decoder instance
 * @param param_reader parameter reader used for reading the desired configuration of the decoder
 * output
 */
void InitializeDecoder(std::unique_ptr<H264Decoder> & decoder,
                       const Aws::Client::ParameterReaderInterface & param_reader)
{
  decoder = std::unique_ptr<H264Decoder>(new H264Decoder());
  if (nullptr != decoder) {
    decoder->Initialize(AV_PIX_FMT_BGR24, param_reader);
  }
}

void ImageCallback(const kinesis_video_msgs::KinesisVideoFrameConstPtr & msg, const H264Decoder * decoder,
                   uint64_t & frame_num, ros::Publisher & pub)
{
  sensor_msgs::Image output_frame;
  thread_local H264DecoderOutput decoder_output(output_frame.data);
  std::vector<uint8_t> joined_data(msg->frame_data.size() + msg->codec_private_data.size());
  std::copy(msg->codec_private_data.begin(), msg->codec_private_data.end(), joined_data.begin());
  std::copy(msg->frame_data.begin(), msg->frame_data.end(), joined_data.begin() + msg->codec_private_data.size());
  H264DecoderInput decoder_input(joined_data);
  decoder_input.frame_dts = msg->decoding_ts;
  decoder_input.frame_pts = msg->presentation_ts;
  decoder_input.frame_duration = msg->duration;
  decoder_input.key_frame = msg->flags & kKeyFrameFlag;
  AwsError retcode = decoder->Decode(decoder_input, decoder_output);
  if (retcode != AWS_ERR_OK) {
    if (retcode == AWS_ERR_NULL_PARAM) {
      AWS_LOG_ERROR(__func__, "Decoder received empty data!");
    } else if (retcode == AWS_ERR_FAILURE) {
      AWS_LOG_ERROR(__func__, "Unknown decoding error occurred");
    } else if (retcode == AWS_ERR_EMPTY) {
      AWS_LOG_WARN(__func__, "Decoder returned empty frame");
    }
    return;
  }
  uint64_t stamp_us = 0;
  std::for_each(msg->metadata.begin(), msg->metadata.end(), [&stamp_us](const diagnostic_msgs::KeyValue& kv) {
    if (kv.key == "stamp_us") {
      stamp_us = std::stoul(kv.value);
    }
  });
  output_frame.header.stamp.fromNSec(stamp_us * 1000);
  output_frame.encoding = sensor_msgs::image_encodings::BGR8;
  output_frame.height = decoder_output.height;
  output_frame.width = decoder_output.width;
  output_frame.step = decoder_output.stride;  

  pub.publish(output_frame);
}

void InitializeCommunication(ros::NodeHandle & n,
                             ros::NodeHandle & pn,
                             ros::Subscriber& image_sub,
                             ros::Publisher& pub,
                             std::unique_ptr<H264Decoder>& decoder,
                             uint64_t & frame_num,
                             Aws::Client::Ros1NodeParameterReader & param_reader)
{
  //
  // reading parameters
  //
  H264DecoderNodeParams params;
  GetH264DecoderNodeParams(param_reader, params);


  pub = n.advertise<sensor_msgs::Image>(params.publication_topic,
                                                            params.queue_size);

  //
  // subscribing to topic with callback
  //
  boost::function<void(const kinesis_video_msgs::KinesisVideoFrameConstPtr &)> image_callback;
  image_callback = [&](const kinesis_video_msgs::KinesisVideoFrameConstPtr & msg) -> void {
    if (0 < pub.getNumSubscribers()) {
      if (nullptr == decoder) {
        InitializeDecoder(decoder, param_reader);
      }
      if (nullptr != decoder) {
        ImageCallback(msg, decoder.get(), frame_num, pub);
      }
    } else {
      frame_num = 0;
    }
  };

  image_sub = n.subscribe(params.subscription_topic, params.queue_size, image_callback);
  AWS_LOGSTREAM_INFO(__func__, "subscribed to " << params.subscription_topic << "...");

}

AwsError RunDecoderNode(int argc, char ** argv)
{
  ros::init(argc, argv, "h264_video_decoder");
  ros::NodeHandle n("");
  ros::NodeHandle pn("~");
  
  Aws::Utils::Logging::InitializeAWSLogging(
    Aws::MakeShared<Aws::Utils::Logging::AWSROSLogger>("h264_video_decoder"));
  AWS_LOG_INFO(__func__, "Starting H264 Video Node...");

  ros::Publisher pub;
  ros::Subscriber image_sub;
  std::unique_ptr<H264Decoder> decoder;
  uint64_t frame_num = 0;
  kinesis_video_msgs::KinesisImageMetadata metadata;
  Aws::Client::Ros1NodeParameterReader param_reader;

  InitializeCommunication(n, pn, image_sub, pub,
                          decoder, frame_num, param_reader);
  
  //
  // run the node
  //
  ros::spin();
  AWS_LOG_INFO(__func__, "Shutting down H264 Video Node...");
  Aws::Utils::Logging::ShutdownAWSLogging();
  return AWS_ERR_OK;
}

}  // namespace Kinesis
}  // namespace Aws