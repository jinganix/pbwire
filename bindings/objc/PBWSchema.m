#import "PBWSchema.h"

#import "PBWError.h"
#import "PBWMessage.h"

NSString *const PBWErrorDomain = @"PBWErrorDomain";

@implementation PBWSchema {
    /* Owned by this object; dies in -dealloc. Messages retain the schema, so
     * the descriptor a message points at stays valid. */
    pbw_schema *_schema;
}

+ (nullable instancetype)schemaFromFile:(NSString *)path error:(NSError **)error
{
    NSData *data = [NSData dataWithContentsOfFile:path options:0 error:error];

    if (data == nil) {
        return nil; /* Foundation's file error is the useful one here. */
    }
    return [self schemaFromData:data error:error];
}

+ (nullable instancetype)schemaFromData:(NSData *)data error:(NSError **)error
{
    pbw_schema *schema = NULL;
    pbw_status status = pbw_descriptor_set_load(data.bytes, data.length, &schema);

    if (status != PBW_OK) {
        const char *reason = pbw_schema_error(schema);
        NSString *detail = reason != NULL ? @(reason) : nil;

        pbw_schema_free(schema);
        PBWSetError(error, status, detail);
        return nil;
    }
    return [[self alloc] initWithCSchema:schema];
}

- (instancetype)initWithCSchema:(pbw_schema *)schema
{
    self = [super init];
    if (self == nil) {
        return nil;
    }
    _schema = schema;
    return self;
}

- (void)dealloc
{
    pbw_schema_free(_schema);
    _schema = NULL;
}

- (nullable PBWMessage *)messageByName:(NSString *)name
{
    const pbw_descriptor *descriptor;
    const char *utf8 = name.UTF8String;

    if (utf8 == NULL) {
        return nil;
    }
    descriptor = pbw_schema_find(_schema, utf8);
    if (descriptor == NULL || pbw_descriptor_is_enum(descriptor)) {
        return nil;
    }
    return [PBWMessage messageWithDescriptor:descriptor schema:self error:NULL];
}

- (nonnull NSArray<NSString *> *)serviceNames
{
    size_t count = pbw_schema_service_count(_schema);
    NSMutableArray<NSString *> *out =
        [NSMutableArray arrayWithCapacity:count];
    size_t i;

    for (i = 0; i < count; i++) {
        const pbw_service *svc = pbw_schema_service_at(_schema, i);
        const char *name = svc != NULL ? pbw_service_name(svc) : NULL;

        if (name != NULL) {
            [out addObject:@(name)];
        }
    }
    return out;
}

- (const pbw_service *)serviceForName:(NSString *)service
{
    const char *utf8 = service.UTF8String;

    return utf8 != NULL ? pbw_schema_service_find(_schema, utf8) : NULL;
}

- (nullable NSArray<NSString *> *)methodNamesForService:(NSString *)service
{
    const pbw_service *svc = [self serviceForName:service];
    size_t count;
    NSMutableArray<NSString *> *out;
    size_t i;

    if (svc == NULL) {
        return nil;
    }
    count = pbw_service_method_count(svc);
    out = [NSMutableArray arrayWithCapacity:count];
    for (i = 0; i < count; i++) {
        const pbw_method *method = pbw_service_method_at(svc, i);
        const char *name = method != NULL ? pbw_method_name(method) : NULL;

        if (name != NULL) {
            [out addObject:@(name)];
        }
    }
    return out;
}

- (const pbw_method *)methodForService:(NSString *)service
                                method:(NSString *)method
{
    const pbw_service *svc = [self serviceForName:service];
    const char *utf8 = method.UTF8String;

    if (svc == NULL || utf8 == NULL) {
        return NULL;
    }
    return pbw_service_method_find(svc, utf8);
}

- (nullable NSString *)inputTypeForService:(NSString *)service
                                    method:(NSString *)method
{
    const pbw_method *found = [self methodForService:service method:method];
    const char *type = found != NULL ? pbw_method_input_type(found) : NULL;

    return type != NULL ? @(type) : nil;
}

- (nullable NSString *)outputTypeForService:(NSString *)service
                                     method:(NSString *)method
{
    const pbw_method *found = [self methodForService:service method:method];
    const char *type = found != NULL ? pbw_method_output_type(found) : NULL;

    return type != NULL ? @(type) : nil;
}

@end
