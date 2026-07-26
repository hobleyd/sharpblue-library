Pod::Spec.new do |s|
  s.name             = 'sharpblue_library'
  s.version          = '0.1.0'
  s.summary          = 'Cross-platform HTML/CSS3 widget using native browser engines.'
  s.description      = 'WKWebView-backed HTML widget for macOS.'
  s.homepage         = 'https://github.com/nightmail/html_view'
  s.license          = { :type => 'MIT' }
  s.author           = { 'Nightmail' => 'dev@nightmail.com.au' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'
  s.platform = :osx, '10.14'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
