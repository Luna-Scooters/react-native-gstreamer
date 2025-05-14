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
  
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386 arm64',
    'HEADER_SEARCH_PATHS' => '"${HOME}/Library/Developer/GStreamer/iPhone.sdk/GStreamer.framework/Headers"',
    'LD_RUNPATH_SEARCH_PATHS' => ['"${HOME}/Library/Developer/GStreamer/iPhone.sdk"', '"${HOME}/Library/Developer/GStreamer/iPhone.sdk/GStreamer.framework/Libraries"'],
    'OTHER_LDFLAGS' => '" -L${HOME}/Library/Developer/GStreamer/iPhone.sdk/GStreamer.framework/Libraries -F${HOME}/Library/Developer/GStreamer/iPhone.sdk -framework GStreamer "'
  }
  
  s.vendored_frameworks = "GStreamer.framework" 
  s.xcconfig = { 'FRAMEWORK_SEARCH_PATHS' => '"${HOME}/Library/Developer/GStreamer/iPhone.sdk"' }

  # Properly specify architecture requirements
  s.user_target_xcconfig = { 'VALID_ARCHS' => 'arm64' }
  
  s.dependency "React-Core"
end