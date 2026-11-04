#import "PBWMessage.h"

#import "PBWError.h"
#import "PBWSchema.h"

@implementation PBWMessage {
    PBWSchema *_schema; /* strong: owns the descriptor below */
    const pbw_descriptor *_descriptor;
    pbw_message *_message; /* the C tree; freed here when _ownsMessage */
    BOOL _ownsMessage;
    PBWMessage *_parent; /* strong: a view must not outlive its storage */
}

+ (nullable instancetype)messageWithDescriptor:(const pbw_descriptor *)descriptor
                                        schema:(PBWSchema *)schema
                                         error:(NSError **)error
{
    pbw_message *message = pbw_message_new(descriptor);

    if (message == NULL) {
        PBWSetError(error, PBW_ERR_NOMEM, nil);
        return nil;
    }
    return [[self alloc] initWithDescriptor:descriptor
                                     schema:schema
                                    message:message
                                      owned:YES
                                     parent:nil];
}

- (instancetype)initWithDescriptor:(const pbw_descriptor *)descriptor
                            schema:(PBWSchema *)schema
                           message:(pbw_message *)message
                             owned:(BOOL)owned
                            parent:(PBWMessage *)parent
{
    self = [super init];
    if (self == nil) {
        return nil;
    }
    _descriptor = descriptor;
    _schema = schema;
    _message = message;
    _ownsMessage = owned;
    _parent = parent;
    return self;
}

- (void)dealloc
{
    /* Frees the whole decoded tree, nested messages included; a nested view
     * (owned == NO) frees nothing, its parent does. */
    if (_ownsMessage) {
        pbw_message_free(_message);
    }
    _message = NULL;
}

- (NSString *)messageName
{
    const char *name = pbw_descriptor_name(_descriptor);

    return name != NULL ? @(name) : @"";
}

- (const pbw_field *)fieldForName:(NSString *)name
{
    const char *utf8 = name.UTF8String;

    return utf8 != NULL ? pbw_descriptor_find_field_name(_descriptor, utf8) : NULL;
}

- (BOOL)hasField:(NSString *)name
{
    const pbw_field *field = [self fieldForName:name];

    return field != NULL && pbw_message_get(_message, pbw_field_number(field)) != NULL;
}

- (nullable id)valueForField:(NSString *)name
{
    const pbw_field *field = [self fieldForName:name];

    if (field == NULL || pbw_field_label(field) == PBW_LABEL_REPEATED) {
        return nil;
    }
    return [self objectForValue:pbw_message_get(_message, pbw_field_number(field)) field:field];
}

- (NSUInteger)countForField:(NSString *)name
{
    const pbw_field *field = [self fieldForName:name];
    pbw_value *value;

    if (field == NULL || pbw_field_label(field) != PBW_LABEL_REPEATED) {
        return 0;
    }
    value = pbw_message_get(_message, pbw_field_number(field));
    return value == NULL ? 0 : (NSUInteger)pbw_value_count(value);
}

- (nullable id)valueForField:(NSString *)name atIndex:(NSUInteger)index
{
    const pbw_field *field = [self fieldForName:name];
    pbw_value *value;

    if (field == NULL || pbw_field_label(field) != PBW_LABEL_REPEATED) {
        return nil;
    }
    value = pbw_message_get(_message, pbw_field_number(field));
    if (value == NULL || index >= pbw_value_count(value)) {
        return nil;
    }
    return [self objectForValue:pbw_value_at(value, index) field:field];
}

/* One pbw_value as the Objective-C object valueForField: returns. The value
 * may belong to a repeated field's list; it is read only. */
- (nullable id)objectForValue:(pbw_value *)value field:(const pbw_field *)field
{
    const void *bytes;
    size_t len = 0;

    if (value == NULL) {
        return nil;
    }
    switch (pbw_field_type(field)) {
    case PBW_TYPE_STRING:
        bytes = pbw_value_get_bytes(value, &len);
        return [[NSString alloc] initWithBytes:bytes
                                        length:len
                                      encoding:NSUTF8StringEncoding];
    case PBW_TYPE_BYTES:
        bytes = pbw_value_get_bytes(value, &len);
        return [NSData dataWithBytes:bytes length:len];
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return [[PBWMessage alloc] initWithDescriptor:pbw_field_message_type(field)
                                               schema:_schema
                                              message:pbw_value_get_message(value)
                                                owned:NO
                                               parent:self];
    case PBW_TYPE_BOOL:
        return @(pbw_value_get_bool(value) != 0);
    case PBW_TYPE_FLOAT:
        return @(pbw_value_get_float(value));
    case PBW_TYPE_DOUBLE:
        return @(pbw_value_get_double(value));
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32:
        return @(pbw_value_get_uint32(value));
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        return @(pbw_value_get_uint(value));
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        return @(pbw_value_get_int(value));
    default: /* int32, sint32, sfixed32, enum */
        return @(pbw_value_get_int32(value));
    }
}

- (BOOL)setValue:(nullable id)value
        forField:(NSString *)name
           error:(NSError **)error
{
    return [self storeValue:value forField:name allowRepeated:NO error:error];
}

- (BOOL)addValue:(id)value forField:(NSString *)name error:(NSError **)error
{
    return [self storeValue:value forField:name allowRepeated:YES error:error];
}

- (BOOL)storeValue:(nullable id)value
          forField:(NSString *)name
     allowRepeated:(BOOL)allowRepeated
             error:(NSError **)error
{
    const pbw_field *field = [self fieldForName:name];
    pbw_type type;
    pbw_value *stored = NULL;
    pbw_status status;

    if (field == NULL) {
        return PBWSetError(error, PBW_ERR_NOT_FOUND, name);
    }
    type = pbw_field_type(field);
    if (value == nil) {
        pbw_message_clear(_message, pbw_field_number(field));
        return YES;
    }
    if (pbw_field_label(field) == PBW_LABEL_REPEATED && !allowRepeated) {
        return PBWSetError(error, PBW_ERR_INVALID, @"repeated field (use addValue:forField:error:)");
    }
    switch (type) {
    case PBW_TYPE_STRING: {
        const char *utf8;

        if (![value isKindOfClass:[NSString class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        utf8 = ((NSString *)value).UTF8String;
        stored = pbw_value_new_string(utf8 != NULL ? utf8 : "");
        break;
    }
    case PBW_TYPE_BYTES: {
        NSData *data;

        if (![value isKindOfClass:[NSData class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        data = value;
        stored = pbw_value_new_bytes(data.bytes, data.length);
        break;
    }
    case PBW_TYPE_BOOL:
        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        stored = pbw_value_new_bool([value boolValue]);
        break;
    case PBW_TYPE_FLOAT:
        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        stored = pbw_value_new_float([value floatValue]);
        break;
    case PBW_TYPE_DOUBLE:
        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        stored = pbw_value_new_double([value doubleValue]);
        break;
    case PBW_TYPE_INT32:
    case PBW_TYPE_SINT32:
    case PBW_TYPE_SFIXED32:
    case PBW_TYPE_ENUM: {
        long long number;

        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        number = [value longLongValue];
        if (number < INT32_MIN || number > INT32_MAX) {
            return PBWSetError(error, PBW_ERR_TYPE, @"32-bit field out of range");
        }
        /* Signed types are stored sign extended, like the C ABI documents. */
        stored = pbw_value_new_uint((uint64_t)(int64_t)number);
        break;
    }
    case PBW_TYPE_UINT32:
    case PBW_TYPE_FIXED32: {
        unsigned long long number;

        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        number = [value unsignedLongLongValue];
        if (number > UINT32_MAX) {
            return PBWSetError(error, PBW_ERR_TYPE, @"32-bit field out of range");
        }
        stored = pbw_value_new_uint(number);
        break;
    }
    case PBW_TYPE_INT64:
    case PBW_TYPE_SINT64:
    case PBW_TYPE_SFIXED64:
        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        stored = pbw_value_new_int([value longLongValue]);
        break;
    case PBW_TYPE_UINT64:
    case PBW_TYPE_FIXED64:
        if (![value isKindOfClass:[NSNumber class]]) {
            return PBWSetError(error, PBW_ERR_TYPE, name);
        }
        stored = pbw_value_new_uint([value unsignedLongLongValue]);
        break;
    case PBW_TYPE_MESSAGE:
    case PBW_TYPE_GROUP:
        return PBWSetError(error, PBW_ERR_TYPE, @"use addMessageForField:");
    }
    if (stored == NULL) {
        return PBWSetError(error, PBW_ERR_NOMEM, name);
    }
    status = pbw_message_set(_message, pbw_field_number(field), stored);
    if (status != PBW_OK) {
        /* Ownership only moves on success (pbw_message_set contract). */
        pbw_value_free(stored);
        return PBWSetError(error, status, name);
    }
    return YES;
}

- (nullable PBWMessage *)messageForField:(NSString *)name
{
    id value = [self valueForField:name];

    return [value isKindOfClass:[PBWMessage class]] ? value : nil;
}

- (nullable PBWMessage *)addMessageForField:(NSString *)name
                                      error:(NSError **)error
{
    const pbw_field *field = [self fieldForName:name];
    pbw_message *nested;

    if (field == NULL) {
        PBWSetError(error, PBW_ERR_NOT_FOUND, name);
        return nil;
    }
    if (pbw_field_type(field) != PBW_TYPE_MESSAGE &&
        pbw_field_type(field) != PBW_TYPE_GROUP) {
        PBWSetError(error, PBW_ERR_TYPE, name);
        return nil;
    }
    nested = pbw_message_set_message(_message, pbw_field_number(field));
    if (nested == NULL) {
        /* This wrapper never builds in a context arena, so the only failure is
         * an allocation failure. */
        PBWSetError(error, PBW_ERR_NOMEM, name);
        return nil;
    }
    return [[PBWMessage alloc] initWithDescriptor:pbw_field_message_type(field)
                                           schema:_schema
                                          message:nested
                                            owned:NO
                                           parent:self];
}

- (nullable NSData *)encode:(NSError **)error
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    pbw_status status = pbw_encode(_message, &bytes, &len);

    if (status != PBW_OK) {
        PBWSetError(error, status, nil);
        return nil;
    }
    if (len == 0) {
        /* -[NSData initWithBytesNoCopy:length:freeWhenDone:] returns the shared
         * empty object and leaks the buffer when the length is 0. */
        pbw_free(bytes);
        return [NSData data];
    }
    /* Hand the malloc'd buffer over instead of copying it: the encoder already
     * wrote the exact bytes, and -[NSData dataWithBytes:length:] copies 74 KB
     * in 7 us where the transfer costs nothing. pbw_encode transfers ownership,
     * so freeWhenDone:YES is exactly the C contract. */
    return [NSData dataWithBytesNoCopy:bytes length:len freeWhenDone:YES];
}

- (nullable PBWMessage *)decode:(NSData *)data error:(NSError **)error
{
    pbw_message *decoded = NULL;
    pbw_status status = pbw_decode(_descriptor, data.bytes, data.length, &decoded);

    if (status != PBW_OK) {
        PBWSetError(error, status, nil);
        return nil;
    }
    return [[PBWMessage alloc] initWithDescriptor:_descriptor
                                           schema:_schema
                                          message:decoded
                                            owned:YES
                                           parent:nil];
}

@end
