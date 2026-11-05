#!/usr/bin/env ruby
# frozen_string_literal: true

# Google conformance testee driven by the pbwire Ruby binding.
#
# It speaks the runner's wire protocol (4-byte little-endian length prefix, then
# a ConformanceRequest / ConformanceResponse) and, because the binding is
# schema-driven, it needs no per-message code: the request names the message
# type and the descriptor set answers with the schema.
#
# Supported: binary, JSON and text in -> binary, JSON or text out, through
# the binding's printers and parsers (Message#to_json/to_text/from_json/
# from_text). JSPB answers `skipped`, like every other testee here.
#
# decode() validates the whole payload in one scan and keeps the bytes it was
# parsed from, so encode() hands them back byte for byte. Verdicts are therefore
# what the suite tests here: a request must be accepted exactly when the
# reference implementation accepts it.
#
#   ruby testee.rb build/conformance.desc     # from the repo root
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'

PROTOBUF = 1
JSON = 2
JSPB = 3
TEXT = 4

def read_request(stream)
  header = stream.read(4)
  return nil if header.nil? || header.bytesize < 4

  size = header.unpack1('V')
  body = stream.read(size)
  return nil if body.nil? || body.bytesize < size

  body
end

def write_response(stream, payload)
  stream.write([payload.bytesize].pack('V'))
  stream.write(payload)
  stream.flush
end

# Test categories the runner sends (conformance.proto): JSON input with
# unknown fields only parses when the category asks to ignore them.
JSON_IGNORE_UNKNOWN = 3

# One input payload wins: binary beats JSON beats text, matching the
# reference testee's precedence. Returns a message holding the parsed tree:
# binary decodes materialize (canonical re-encode, like the C testee),
# JSON/text parses already arrive as trees.
def parse_input(message, request)
  payload = request['protobuf_payload']
  return message.decode(payload).to_builder unless payload.nil?

  json = request['json_payload']
  unless json.nil?
    ignore = request['test_category'] == JSON_IGNORE_UNKNOWN
    return message.from_json(json, ignore)
  end

  text = request['text_payload']
  return message.from_text(text) unless text.nil?

  raise Pbwire::Error, 'no input payload in the request'
end

if ARGV.empty?
  warn 'usage: testee.rb <conformance.desc>'
  exit 2
end
schema = Pbwire::Schema.from_file(ARGV[0])
request_type = schema.message('conformance.ConformanceRequest')
$stdin.binmode
$stdout.binmode

loop do
  body = read_request($stdin)
  break if body.nil?

  response_type = schema.message('conformance.ConformanceResponse')
  begin
    request = request_type.decode(body)
    wanted = request['requested_output_format'] || 0
    name = request['message_type']
    if wanted == JSPB
      response_type['skipped'] = 'JSPB is not supported'
    else
      begin
        message = schema.message(name)
      rescue Pbwire::Error
        response_type['runtime_error'] = "unknown message type #{name.inspect}"
      else
        begin
          parsed = parse_input(message, request)
        rescue Pbwire::Error => e
          response_type['parse_error'] = e.message
        else
          begin
            case wanted
            when PROTOBUF
              response_type['protobuf_payload'] = parsed.encode
            when JSON
              response_type['json_payload'] = parsed.to_json
            when TEXT
              print_unknown = request['print_unknown_fields'] == true
              response_type['text_payload'] = parsed.to_text(print_unknown)
            else
              response_type['skipped'] = "output format #{wanted} is not supported"
            end
          rescue Pbwire::Error => e
            response_type['serialize_error'] = e.message
          end
        end
      end
    end
  rescue StandardError => e
    response_type['runtime_error'] = "cannot handle request: #{e.class}: #{e.message}"
  end

  begin
    payload = response_type.encode
  rescue StandardError => e
    payload = schema.message('conformance.ConformanceResponse').tap do |r|
      r['runtime_error'] = "cannot encode response: #{e.class}: #{e.message}"
    end.encode
  end
  write_response($stdout, payload)
end
