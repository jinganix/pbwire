/* Shared error plumbing for the Objective-C binding: every pbw_status leaves
 * the wrapper as an NSError with the status as its code, so a caller never has
 * to look at the C ABI to find out what happened. */

#ifndef PBW_ERROR_H
#define PBW_ERROR_H

#import <Foundation/Foundation.h>

#include "pbwire.h"

/* PBWSchema.h also declares this (users match on it). */
extern NSString *const PBWErrorDomain;

/* Writes an NSError for status into *error (when error is not NULL) and
 * returns NO, so a failing method is one line: `return PBWSetError(...)`.
 * detail may be NULL; it carries pbw_schema_error() or the field name. */
static inline BOOL
PBWSetError(NSError **error, pbw_status status, NSString *detail)
{
    if (error != NULL) {
        const char *text = pbw_strerror(status);
        NSString *message = text != NULL ? @(text) : @"pbwire error";

        if (detail.length > 0) {
            message = [message stringByAppendingFormat:@": %@", detail];
        }
        *error = [NSError errorWithDomain:PBWErrorDomain
                                     code:status
                                 userInfo:@{NSLocalizedDescriptionKey : message}];
    }
    return NO;
}

#endif /* PBW_ERROR_H */
