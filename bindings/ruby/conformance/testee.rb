#!/usr/bin/env ruby
# frozen_string_literal: true

# Google conformance testee driven by the pbwire Ruby binding.
#
# It speaks the runner's wire protocol (4-byte little-endian length prefix, then
# a ConformanceRequest / ConformanceResponse) and, because the binding is
# schema-driven, it needs no per-message code: the request names the message
# type and the descriptor set answers with the schema.
#
# Supported: binary in -> binary out. JSON/text output needs printers the
# binding does not expose, so those requests answer `skipped` and the runner
# reports them as skipped rather than failed.
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
    if wanted != PROTOBUF
      response_type['skipped'] = "output format #{wanted} is not supported"
    elsif request['protobuf_payload'].nil?
      response_type['skipped'] = 'only binary input is supported'
    else
      begin
        message = schema.message(name)
      rescue Pbwire::Error
        response_type['runtime_error'] = "unknown message type #{name.inspect}"
      else
        begin
          parsed = message.decode(request['protobuf_payload'])
        rescue Pbwire::Error => e
          response_type['parse_error'] = e.message
        else
          response_type['protobuf_payload'] = parsed.encode
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
