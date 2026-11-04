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

@end
