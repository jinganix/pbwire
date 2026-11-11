#!/usr/bin/env ruby
# frozen_string_literal: true

# Self-check for the Ruby binding: plain asserts, no test framework, no gems.
# Run it with test/run.sh (builds libpbwire first) or directly with the library
# already built: ruby -Ilib test/test_pbwire.rb
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'

DATA = File.expand_path('data/sample.desc', __dir__)
SAMPLE = 'pbwire.ruby.Sample'
INNER = 'pbwire.ruby.Inner'

# protoc --encode=pbwire.ruby.Sample for the message built by
# `reference_message` below (see test/data/sample.proto). Pinned so the test
# proves byte-level parity with the reference implementation without needing
# protoc at test time.
WIRE_HEX = '08f9ffffffffffffffff0110cb89ec8ff72318ffffffff0f20ffffffffffffffffff01' \
           '2801350000c03f3900000000000002404205616c6963654a0200ff50015a080a066e65' \
           '73746564620dfbffffffffffffffff0100ac026a01616a02626272070a05666972737472' \
           '080a067365636f6e64'

class AssertionFailed < StandardError; end

def assert(condition, message)
  raise AssertionFailed, message unless condition
end

def assert_equal(expected, actual, message)
  assert expected == actual, "#{message}: expected #{expected.inspect}, got #{actual.inspect}"
end

# The exception the block raises, or nil. A different class propagates and fails
# the check, which is what we want: it means the failure path changed.
def raised(klass)
  yield
  nil
rescue klass => e
  e
end

def schema
  $schema ||= Pbwire::Schema.from_file(DATA)
end

def nested(name)
  msg = schema.message(INNER)
  msg['s'] = name
  msg
end

# All the value mappings at once, set in an order that is not field order: the
# encoder writes field order, so this also proves set order does not leak into
# the bytes.
def reference_message
  msg = schema.message(SAMPLE)
  msg['inners'] = [nested('first'), nested('second')]
  msg['tags'] = %w[a bb]
  msg['nums'] = [-5, 0, 300]
  msg['inner'] = nested('nested')
  msg['color'] = 1
  msg['blob'] = "\x00\xff".b
  msg['name'] = 'alice'
  msg['f64'] = 2.25
  msg['f32'] = 1.5
  msg['flag'] = true
  msg['u64'] = 18_446_744_073_709_551_615
  msg['u32'] = 4_294_967_295
  msg['i64'] = 1_234_567_890_123
  msg['i32'] = -7
  msg
end

CHECKS = {}
def check(name, &block)
  CHECKS[name] = block
end

check 'descriptor set loads from a file' do
  assert_equal SAMPLE, schema.message(SAMPLE).type, 'message name'
  assert_equal INNER, schema.message(INNER).type, 'nested message name'
end

check 'descriptor set loads from bytes' do
  loaded = Pbwire::Schema.from_bytes(File.binread(DATA))
  assert_equal SAMPLE, loaded.message(SAMPLE).type, 'message name'
  loaded.close
end

check 'unknown message name raises' do
  assert raised(Pbwire::Error) { schema.message('pbwire.ruby.Nope') }, 'expected an error'
end

check 'malformed descriptor set raises the schema error' do
  error = raised(Pbwire::Error) { Pbwire::Schema.from_bytes("\x00\x01not a descriptor set") }
  assert error, 'expected an error'
  assert error.message.include?('descriptor set rejected'), "unexpected message: #{error.message}"
end

check 'round trip of every supported field type' do
  wire = reference_message.encode
  back = schema.message(SAMPLE).decode(wire)
  assert_equal(-7, back['i32'], 'i32')
  assert_equal 1_234_567_890_123, back['i64'], 'i64'
  assert_equal 4_294_967_295, back['u32'], 'u32'
  assert_equal 18_446_744_073_709_551_615, back['u64'], 'u64'
  assert_equal true, back['flag'], 'bool'
  assert_equal 1.5, back['f32'], 'float'
  assert_equal 2.25, back['f64'], 'double'
  assert_equal 'alice', back['name'], 'string'
  assert_equal "\x00\xff".b, back['blob'], 'bytes'
  assert_equal 1, back['color'], 'enum'
end

check 'encode matches the protoc reference bytes' do
  assert_equal WIRE_HEX, reference_message.encode.unpack1('H*'), 'wire hex'
end

check 'decoding the protoc bytes reads every field' do
  back = schema.message(SAMPLE).decode([WIRE_HEX].pack('H*'))
  assert_equal(-7, back['i32'], 'i32')
  assert_equal 1_234_567_890_123, back['i64'], 'i64'
  assert_equal 4_294_967_295, back['u32'], 'u32'
  assert_equal 18_446_744_073_709_551_615, back['u64'], 'u64'
  assert_equal true, back['flag'], 'bool'
  assert_equal 1.5, back['f32'], 'float'
  assert_equal 2.25, back['f64'], 'double'
  assert_equal 'alice', back['name'], 'string'
  assert_equal "\x00\xff".b, back['blob'], 'bytes'
  assert_equal 1, back['color'], 'enum'
  assert_equal 'nested', back['inner']['s'], 'nested'
  assert_equal [-5, 0, 300], back['nums'], 'packed repeated scalar'
  assert_equal %w[a bb], back['tags'], 'repeated string'
  assert_equal %w[first second], back['inners'].map { |m| m['s'] }, 'repeated message'
end

check 're-encode of a decoded message is byte exact' do
  wire = reference_message.encode
  assert_equal wire, schema.message(SAMPLE).decode(wire).encode, 're-encode'
end

check 'unknown fields survive a decode/encode round trip' do
  wire = [WIRE_HEX].pack('H*') + "\xF8\x06\x01".b # field 111, varint 1
  back = schema.message(SAMPLE).decode(wire)
  assert_equal wire, back.encode, 'unknown field bytes'
  assert raised(Pbwire::Error) { back[111] }, 'unknown field lookup'
end

check 'absent fields read as nil, repeated as an empty Array' do
  msg = schema.message(SAMPLE)
  %w[i32 i64 u32 u64 flag f32 f64 name blob color inner].each do |name|
    assert_equal nil, msg[name], "unset #{name}"
  end
  %w[nums tags inners].each do |name|
    assert_equal [], msg[name], "unset #{name}"
  end
  msg['name'] = 'alice'
  assert_equal 'alice', msg['name'], 'set name'
  assert_equal nil, msg['i32'], 'still absent i32'
end

check 'fields are addressable by name, Symbol or number' do
  msg = schema.message(SAMPLE)
  msg.set(8, 'alice')
  assert_equal 'alice', msg[8], 'get by number'
  assert_equal 'alice', msg['name'], 'get by name'
  assert_equal 'alice', msg[:name], 'get by Symbol'
  assert_equal nil, msg[1], 'absent by number'
  msg[:i32] = -1
  assert_equal(-1, msg['i32'], 'set by Symbol')
end

check 'a decoded message does not follow later writes to its input' do
  wire = reference_message.encode
  back = schema.message(SAMPLE).decode(wire)
  wire.replace("\x00".b * wire.bytesize)
  assert_equal '@@@', back['name'].sub('alice', '@@@'), 'name survived the input rewrite'
  assert_equal 'nested', back['inner']['s'], 'nested survived the input rewrite'
end

check 'nested message round trip' do
  inner = nested('nested')
  msg = schema.message(SAMPLE)
  msg['inner'] = inner
  assert inner.closed?, 'the nested message moved into the parent'
  assert_equal 'nested', msg['inner']['s'], 'nested field'
  back = schema.message(SAMPLE).decode(msg.encode)
  assert_equal 'nested', back['inner']['s'], 'decoded nested field'
end

check 'groups encode with START_GROUP framing and decode as messages' do
  gschema = Pbwire::Schema.from_file('bindings/c/tests/data/grouptest.desc')
  gtype = 'structtest.GroupTest'
  inner = gschema.message('structtest.GroupTest.Inner')
  inner['x'] = 5
  inner['s'] = 'g'
  item1 = gschema.message('structtest.GroupTest.Item')
  item1['v'] = 1
  item2 = gschema.message('structtest.GroupTest.Item')
  item2['v'] = -2
  msg = gschema.message(gtype)
  msg['item'] = [item1, item2]
  msg['id'] = 9
  msg['inner'] = inner
  wire = msg.encode
  assert_equal File.binread('bindings/c/tests/data/grouptest.bin'), wire,
               'group bytes equal protoc'
  back = gschema.message(gtype).decode(wire)
  assert_equal 5, back['inner']['x'], 'singular group'
  assert_equal 'g', back['inner']['s'], 'singular group string'
  assert_equal [1, -2], back['item'].map { |m| m['v'] }, 'repeated group'
  assert_equal 9, back['id'], 'sibling field'
  assert_equal wire, back.encode, 'group re-encode byte exact'
  merged = gschema.message(gtype).decode("\x0b\x10\x05\x0c\x0b\x1a\x01g\x0c".b)
  assert_equal 5, merged['inner']['x'], 'singular group merges x'
  assert_equal 'g', merged['inner']['s'], 'singular group merges s'
end

check 'explicit false survives the wire' do
  msg = schema.message(SAMPLE)
  msg['flag'] = false
  assert_equal false, msg['flag'], 'before encode'
  assert_equal false, schema.message(SAMPLE).decode(msg.encode)['flag'], 'after decode'
end

check 'unknown fields raise' do
  msg = schema.message(SAMPLE)
  assert raised(Pbwire::Error) { msg['nope'] = 1 }, 'set unknown name'
  assert raised(Pbwire::Error) { msg.get(99) }, 'get unknown number'
end

check 'a nested field only accepts a message of its own type' do
  assert raised(TypeError) { schema.message(SAMPLE)['inner'] = 5 }, 'expected TypeError'
  assert raised(TypeError) { schema.message(SAMPLE)['inner'] = schema.message(SAMPLE) },
         'expected TypeError for a message of another type'
end

check 'repeated fields round trip and assign as a whole' do
  msg = schema.message(SAMPLE)
  msg['nums'] = [1, 2, 3]
  msg['tags'] = %w[x y]
  msg['inners'] = [nested('a'), nested('b')]
  assert_equal [1, 2, 3], msg['nums'], 'before encode'
  back = schema.message(SAMPLE).decode(msg.encode)
  assert_equal [1, 2, 3], back['nums'], 'packed repeated scalar'
  assert_equal %w[x y], back['tags'], 'repeated string'
  assert_equal %w[a b], back['inners'].map { |m| m['s'] }, 'repeated message'

  msg['nums'] = [9]
  assert_equal [9], msg['nums'], 'assignment replaces'
  msg['nums'] = []
  assert_equal [], msg['nums'], 'empty assignment clears'
  assert_equal [], schema.message(SAMPLE).decode(msg.encode)['nums'], 'cleared on the wire'
end

check 'unpacked occurrences of a repeated scalar are accepted' do
  # Field 12 (nums) is packed in the schema, but every protobuf parser also
  # accepts the unpacked form for a packable repeated field.
  back = schema.message(SAMPLE).decode("\x60\x01\x60\x02\x60\x03".b)
  assert_equal [1, 2, 3], back['nums'], 'unpacked repeated scalar'
end

check 'a message can only be handed to one field' do
  inner = nested('once')
  msg = schema.message(SAMPLE)
  msg['inner'] = inner
  assert inner.closed?, 'handed over'
  error = raised(Pbwire::Error) { msg['inner'] = inner }
  assert error && error.message.include?('closed'), "unexpected message: #{error&.message}"

  a = nested('a')
  b = nested('b')
  dup = schema.message(SAMPLE)
  assert raised(Pbwire::Error) { dup['inners'] = [a, b, a] }, 'the same message twice'
  assert_equal [], dup['inners'], 'a rejected assignment leaves the field empty'
  assert raised(Pbwire::Error) { dup['inner'] = dup }, 'a message cannot contain itself'
end

check 'a borrowed sub-message dies with its parent' do
  parent = schema.message(SAMPLE)
  parent['inner'] = nested('child')
  child = parent['inner']
  assert_equal 'child', child['s'], 'live while the parent is'
  parent.close
  error = raised(Pbwire::Error) { child['s'] }
  assert error && error.message.include?('closed'), "unexpected message: #{error&.message}"
  assert raised(Pbwire::Error) { child['s'] = 'x' }, 'set through a closed parent'
end

check 'a decoded message is a read-only view' do
  back = schema.message(SAMPLE).decode([WIRE_HEX].pack('H*'))
  error = raised(Pbwire::Error) { back['name'] = 'bob' }
  assert error && error.message.include?('read-only'), "unexpected message: #{error&.message}"
  error = raised(Pbwire::Error) { back['inner']['s'] = 'bob' }
  assert error && error.message.include?('read-only'), "unexpected message: #{error&.message}"
end

check 'malformed wire data raises' do
  decoder = schema.message(SAMPLE)
  ["\x0a", "\x00\x00", "\x10\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"].each do |bad|
    assert raised(Pbwire::Error) { decoder.decode(bad) }, "expected an error for #{bad.inspect}"
  end
  # A packed run that does not hold a whole number of values.
  assert raised(Pbwire::Error) { decoder.decode("\x62\x01\x80".b) }, 'truncated packed run'
end

check 'randomized build/encode/decode round trips' do
  random = Random.new(20_260_303)
  50.times do |round|
    msg = schema.message(SAMPLE)
    msg['i32'] = random.rand(-2**31...2**31)
    msg['i64'] = random.rand(-2**63...2**63)
    msg['u64'] = random.rand(0...2**64)
    msg['flag'] = random.rand(2).zero?
    msg['f64'] = random.rand * 1_000_000 - 500_000
    msg['name'] = "name-#{round}"
    msg['blob'] = Array.new(random.rand(0..16)) { random.rand(256) }.pack('C*')
    msg['color'] = random.rand(0..3)
    msg['inner'] = nested("kid-#{round}")
    msg['nums'] = Array.new(random.rand(0..32)) { random.rand(-2**31...2**31) }
    msg['tags'] = Array.new(random.rand(0..8)) { |i| "tag-#{i}" }
    msg['inners'] = Array.new(random.rand(0..8)) { |i| nested("in-#{round}-#{i}") }

    back = schema.message(SAMPLE).decode(msg.encode)
    %w[i32 i64 u64 flag f64 name blob color nums tags].each do |field|
      assert_equal msg[field], back[field], "round #{round} #{field}"
    end
    assert_equal msg['inner']['s'], back['inner']['s'], "round #{round} inner"
    assert_equal msg['inners'].map { |m| m['s'] },
                 back['inners'].map { |m| m['s'] }, "round #{round} inners"
  end
end

check 'close is idempotent and use after close raises' do
  msg = schema.message(SAMPLE)
  msg['name'] = 'alice'
  msg.close
  msg.close
  assert msg.closed?, 'closed?'
  assert_equal 'pbwire.ruby.Sample: message is closed',
               (raised(Pbwire::Error) { msg.encode }&.message || 'no error'), 'encode after close'

  loaded = Pbwire::Schema.from_file(DATA)
  loaded.close
  assert loaded.closed?, 'schema closed?'
  assert raised(Pbwire::Error) { loaded.message(SAMPLE) }, 'message after schema close'
end

check 'a schema closes once its last message is gone' do
  loaded = Pbwire::Schema.from_file(DATA)
  msg = loaded.message(SAMPLE)
  loaded.close
  assert loaded.closed?, 'schema reports closed'
  msg['name'] = 'still usable'
  assert_equal 'still usable', msg['name'], 'message survives schema close'
  assert_equal 'still usable', msg.decode(msg.encode)['name'],
               'decode through a live message after schema close'
  msg.close
end

check 'a decoded message keeps its schema alive too' do
  wire = reference_message.encode
  loaded = Pbwire::Schema.from_file(DATA)
  ty = loaded.message(SAMPLE)
  msg = ty.decode(wire)
  loaded.close
  assert loaded.closed?, 'schema reports closed'
  assert_equal 'alice', msg['name'], 'decoded message survives schema close'
  assert msg.to_json.include?('alice'), 'decoded message prints after schema close'
  ty = nil
  GC.start
  assert_equal 'alice', msg['name'], 'decoded message survives schema GC'
end

check 'JSON mapping round-trips through the binding' do
  msg = reference_message
  json = msg.to_json
  assert json.include?('"name":"alice"'), 'string field prints'
  back = schema.message(SAMPLE).from_json(json)
  assert_equal msg.encode, back.encode, 'JSON round trip is wire-equal'
  assert_equal 'alice', back['name'], 'JSON field reads back'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).from_json('{oops') }, 'malformed JSON raises'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).from_json('{"nope":1}') }, 'unknown JSON field raises'
  strict = schema.message(SAMPLE).from_json('{"nope":1}', true)
  assert strict['name'].nil?, 'ignored unknown field reads as absent'
end

check 'text format round-trips through the binding' do
  msg = reference_message
  text = msg.to_text
  assert text.include?('name: "alice"'), 'string field prints'
  with_unknown = schema.message(SAMPLE).decode(msg.encode + "\x98\x06\x07".b)
  assert with_unknown.to_text.include?('99'), 'to_text prints unknowns by default'
  assert !with_unknown.to_text(false).include?('99'), 'to_text(false) drops unknowns'
  back = schema.message(SAMPLE).from_text(text)
  assert_equal msg.encode, back.encode, 'text round trip is wire-equal'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).from_text('name: ') }, 'malformed text raises'
end

check 'to_builder materializes a canonical tree' do
  inner = schema.message(INNER)
  oneof = inner.decode("\x0a\x01a\x0a\x01b")
  built = oneof.to_builder
  assert_equal 'b', built['s'], 'last value wins after materialize'
  assert_equal built.encode, oneof.to_builder.encode, 'idempotent'
end

check 'deterministic encoding sorts map entries by key' do
  map_schema = Pbwire::Schema.from_file('bindings/c/tests/data/maptest.desc')
  maps = map_schema.message('structtest.Maps')
  unordered = maps.decode("\x0a\x05\x08\x02\x12\x01b\x0a\x05\x08\x01\x12\x01a")
  assert_equal "\x0a\x05\x08\x01\x12\x01a\x0a\x05\x08\x02\x12\x01b",
               unordered.encode_deterministic, 'entries sort by key'
  assert_equal unordered.encode, unordered.decode(unordered.encode).encode,
               'plain encode stable'
end

check 'delimited frames concatenate and decode in a loop' do
  first = reference_message
  second = schema.message(SAMPLE)
  second['name'] = 'bob'
  frames = first.encode_delimited + second.encode_delimited
  names = []
  rest = frames
  until rest.empty?
    decoded, read = schema.message(SAMPLE).decode_delimited(rest)
    assert read.positive?, 'frame reports its size'
    names << decoded['name']
    rest = rest.byteslice(read..)
  end
  assert_equal %w[alice bob], names, 'framed names in order'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).decode_delimited(frames.byteslice(0, 2)) },
              'truncated frame raises'
end

check 'unknown fields survive a decode/encode round trip byte for byte' do
  msg = reference_message
  wire = msg.encode + "\xF8\x06\x01".b
  back = schema.message(SAMPLE).decode(wire)
  assert_equal wire, back.encode, 'unknown field bytes preserved'
  assert_equal wire, schema.message(SAMPLE).decode(wire).to_builder.encode, 'unknown fields survive materialize'
end

failures = 0
CHECKS.each do |name, block|
  begin
    block.call
    puts "ok   #{name}"
  rescue AssertionFailed => e
    failures += 1
    puts "FAIL #{name}: #{e.message}"
  rescue StandardError => e
    failures += 1
    puts "FAIL #{name}: #{e.class}: #{e.message}"
  end
end

ADV = File.expand_path('../../tests/data/adv.desc', __dir__)

check 'visit scans fields without a tree' do
  msg = reference_message
  wire = msg.encode
  events = msg.visit(wire)
  by_num = events.to_h { |n, _, _| [n, true] }
  name_ev = events.find { |n, _, _| n == 8 }
  assert name_ev, 'name field visited'
  assert_equal 2, name_ev[1], 'name wire type'
  assert_equal 'alice', name_ev[2], 'name value'
  assert raised(Pbwire::Error) { msg.visit(wire, { max_bytes: 4 }) }, 'visit honors max_bytes'
end

check 'encoded_size matches and encode_to honors a fixed cap' do
  msg = reference_message
  wire = msg.encode
  assert_equal wire.bytesize, msg.encoded_size, 'encoded_size'
  assert_equal wire, msg.encode_to(wire.bytesize + 12), 'encode_to sized cap works'
  assert raised(Pbwire::Error) { msg.encode_to(4) }, 'encode_to rejects a short cap'
  full = msg.encode_delimited
  assert_equal full, msg.encode_delimited_to(full.bytesize + 12), 'encode_delimited_to matches'
end

check 'encode_sink streams chunks to the block' do
  msg = reference_message
  wire = msg.encode
  chunks = []
  msg.encode_sink { |c| chunks << c; true }
  assert_equal wire, chunks.join, 'sink gets every byte'
  assert raised(Pbwire::Error) { msg.encode_sink { |_| false } }, 'sink abort raises'
end

check 'limits reject oversized input' do
  msg = reference_message
  wire = msg.encode
  assert raised(Pbwire::Error) { schema.message(SAMPLE).decode(wire, { max_bytes: 4 }) }, 'decode max_bytes'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).from_json('{"name":"x"}', false, { max_bytes: 4 }) }, 'from_json max_bytes'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).from_text('name: "x"', { max_bytes: 4 }) }, 'from_text max_bytes'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).decode_delimited(msg.encode_delimited, { max_bytes: 2 }) }, 'decode_delimited max_bytes'
  assert raised(Pbwire::Error) { Pbwire::Schema.from_bytes(File.binread(DATA), { max_bytes: 4 }) }, 'from_bytes max_bytes'
  assert_equal 'alice', schema.message(SAMPLE).decode(wire, {}).get('name'), 'empty limits mean defaults'
end

check 'services resolve and streaming flags decode' do
  aschema = Pbwire::Schema.from_file(ADV)
  assert_equal ['pbwire.adv.Jobs'], aschema.services, 'services lists Jobs'
  jobs = aschema.service('pbwire.adv.Jobs')
  assert_equal 'pbwire.adv.Jobs', jobs[:name], 'service name'
  assert_equal 2, jobs[:methods].size, 'two methods'
  get = jobs[:methods][0]
  assert_equal 'Get', get[:name], 'method name'
  assert_equal 'pbwire.adv.Job', get[:input_type], 'input type'
  assert_equal 'pbwire.adv.Job', get[:output_type], 'output type'
  assert_equal false, get[:client_streaming], 'unary client streaming'
  assert_equal false, get[:server_streaming], 'unary server streaming'
  watch = jobs[:methods][1]
  assert_equal true, watch[:client_streaming], 'streaming client flag'
  assert_equal true, watch[:server_streaming], 'streaming server flag'
  routed = aschema.resolve('pbwire.adv.Jobs', 'Get')
  assert_equal 'pbwire.adv.Job', routed[:input], 'resolve input'
  assert_equal 'pbwire.adv.Job', routed[:output], 'resolve output'
  assert raised(Pbwire::Error) { aschema.service('pbwire.adv.Nope') }, 'unknown service raises'
  assert raised(Pbwire::Error) { aschema.resolve('pbwire.adv.Jobs', 'Nope') }, 'unknown method raises'
end

check 'well-known-type helpers round trip' do
  aschema = Pbwire::Schema.from_file(ADV)
  payload = aschema.message('pbwire.adv.Job').tap { |m| m['id'] = '7' }.encode
  packed = aschema.message('google.protobuf.Any').any_pack('type.googleapis.com/pbwire.adv.Job', payload)
  assert_equal 'type.googleapis.com/pbwire.adv.Job', packed.any_type_url, 'any type url'
  assert_equal '7', aschema.message('pbwire.adv.Job').decode(packed.any_payload).get('id'), 'any payload'
  ts = aschema.message('google.protobuf.Timestamp').timestamp_from_millis(1501)
  assert_equal 1501, ts.timestamp_millis, 'timestamp millis round trip'
  du = aschema.message('google.protobuf.Duration').duration_from_millis(-2500)
  assert_equal(-2500, du.duration_millis, 'duration millis round trip')
  fm = aschema.message('google.protobuf.FieldMask').from_json('{"paths":["a.b","c"]}')
  assert_equal %w[a.b c], fm.fieldmask_paths, 'fieldmask paths read'
  assert raised(Pbwire::Error) { schema.message(SAMPLE).any_pack('t', 'p') }, 'any_pack rejects a non-Any type'
end

check 'pool and context recycle arenas' do
  msg = reference_message
  wire = msg.encode
  ctx = Pbwire::Context.new
  assert_equal wire, ctx.encode(schema.message(SAMPLE).tap { |m| m['name'] = 'alice' }), 'context encode matches'
  borrowed = ctx.decode(schema, SAMPLE, wire)
  assert_equal wire, borrowed.encode, 'context decode reads back'
  assert raised(Pbwire::Error) { borrowed['name'] = 'bob' }, 'context tree is read-only'
  ctx.reset
  assert raised(Pbwire::Error) { borrowed.encode }, 'reset invalidates the borrow'
  assert ctx.used >= 0, 'used reports bytes held'
  ctx.retain_cap = 1024
  assert_equal 1024, ctx.retain_cap, 'retain cap reads back'
  pool = Pbwire::Pool.new(2, 0)
  pc = pool.acquire
  assert_equal 0, pool.idle, 'acquire pops the freelist'
  pc.encode(schema.message(SAMPLE).tap { |m| m['name'] = 'alice' })
  pool.release(pc)
  assert_equal 1, pool.idle, 'release parks the context'
  assert raised(Pbwire::Error) { pc.encode(msg) }, 'released context raises'
  assert raised(Pbwire::Error) { pool.release(pc) }, 'double release raises'
end

puts "#{CHECKS.size - failures}/#{CHECKS.size} checks passed"
exit(failures.zero? ? 0 : 1)
