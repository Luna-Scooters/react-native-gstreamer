require 'json'

package = JSON.parse(File.read(File.join(__dir__, 'package.json')))

Pod::Spec.new do |s|
  s.name         = "react-native-gstreamer"
  s.version      = package['version']
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.author       = package["author"]
  s.platform     = :ios, "13.0"
  s.source       = { :git => "#{s.homepage}.git", :tag => "#{s.version}" }

  s.source_files = "ios/RCTGstPlayer/*.{h,m}", "common/*.{h,c}"
  s.public_header_files = "ios/RCTGstPlayer/*.h"
  
  gst_root = '$(PODS_ROOT)/../../GStreamer-iOS/GStreamer.xcframework'
  gst_device = "#{gst_root}/ios-arm64"
  gst_sim    = "#{gst_root}/ios-arm64_x86_64-simulator"

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    "HEADER_SEARCH_PATHS[sdk=iphoneos*]"        => "\"#{gst_device}/Headers\"",
    "HEADER_SEARCH_PATHS[sdk=iphonesimulator*]" => "\"#{gst_sim}/Headers\"",
  }
  s.dependency "React-Core"
end