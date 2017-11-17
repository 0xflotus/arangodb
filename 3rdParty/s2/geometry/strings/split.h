// Copyright 2008 and onwards Google, Inc.
//
// Functions for splitting and parsing strings.  Functions may be migrated
// to this file from strutil.h in the future.
//
#ifndef STRINGS_SPLIT_H_
#define STRINGS_SPLIT_H_

#include <string>
#include <vector>
#include <set>
#include <utility>
#include <unordered_map>
#include <unordered_set>

// ----------------------------------------------------------------------
// SplitStringAllowEmpty()
// SplitStringToHashsetAllowEmpty()
// SplitStringToSetAllowEmpty()
// SplitStringToHashmapAllowEmpty()

//    Split a string using one or more character delimiters, presented
//    as a nul-terminated c string. Append the components to 'result'.
//    If there are consecutive delimiters, this function will return
//    corresponding empty strings.
//
//    If "full" is the empty string, yields an empty string as the only value.
// ----------------------------------------------------------------------
void SplitStringAllowEmpty(const std::string& full, const char* delim,
                           std::vector<std::string>* res);
void SplitStringToHashsetAllowEmpty(const std::string& full, const char* delim,
                                    std::unordered_set<std::string>* res);
void SplitStringToSetAllowEmpty(const std::string& full, const char* delim,
                                std::set<std::string>* res);
// The even-positioned (0-based) components become the keys for the
// odd-positioned components that follow them. When there is an odd
// number of components, the value for the last key will be unchanged
// if the key was already present in the hash table, or will be the
// empty string if the key is a newly inserted key.
void SplitStringToHashmapAllowEmpty(const std::string& full, const char* delim,
                            std::unordered_map<std::string, std::string>* result);

// ----------------------------------------------------------------------
// SplitStringUsing()
// SplitStringToHashsetUsing()
// SplitStringToSetUsing()
// SplitStringToHashmapUsing()

//    Split a string using one or more character delimiters, presented
//    as a nul-terminated c string. Append the components to 'result'.
//    If there are consecutive delimiters, this function skips over
//    all of them.
// ----------------------------------------------------------------------
void SplitStringUsing(const std::string& full, const char* delim,
                      std::vector<std::string>* res);
void SplitStringToHashsetUsing(const std::string& full, const char* delim,
                               std::unordered_set<std::string>* res);
void SplitStringToSetUsing(const std::string& full, const char* delim,
                           std::set<std::string>* res);
// The even-positioned (0-based) components become the keys for the
// odd-positioned components that follow them. When there is an odd
// number of components, the value for the last key will be unchanged
// if the key was already present in the hash table, or will be the
// empty string if the key is a newly inserted key.
void SplitStringToHashmapUsing(const std::string& full, const char* delim,
                      std::unordered_map<std::string, std::string>* result);

// ----------------------------------------------------------------------
// SplitOneIntToken()
// SplitOneInt32Token()
// SplitOneUint32Token()
// SplitOneInt64Token()
// SplitOneUint64Token()
// SplitOneDoubleToken()
// SplitOneFloatToken()
//   Parse a single "delim" delimited number from "*source" into "*value".
//   Modify *source to point after the delimiter.
//   If no delimiter is present after the number, set *source to NULL.
//
//   If the start of *source is not an number, return false.
//   If the int is followed by the null character, return true.
//   If the int is not followed by a character from delim, return false.
//   If *source is NULL, return false.
//
//   They cannot handle decimal numbers with leading 0s, since they will be
//   treated as octal.
// ----------------------------------------------------------------------
bool SplitOneIntToken(const char** source, const char* delim,
                      int* value);
bool SplitOneInt32Token(const char** source, const char* delim,
                        int32_t* value);
bool SplitOneUint32Token(const char** source, const char* delim,
                         uint32_t* value);
bool SplitOneInt64Token(const char** source, const char* delim,
                        int64_t* value);
bool SplitOneUint64Token(const char** source, const char* delim,
                         uint64_t* value);
bool SplitOneDoubleToken(const char** source, const char* delim,
                         double* value);
bool SplitOneFloatToken(const char** source, const char* delim,
                        float* value);

// Some aliases, so that the function names are standardized against the names
// of the reflection setters/getters in proto2. This makes it easier to use
// certain macros with reflection when creating custom text formats for protos.

inline bool SplitOneUInt32Token(const char** source, const char* delim,
                         uint32_t* value) {
  return SplitOneUint32Token(source, delim, value);
}

inline bool SplitOneUInt64Token(const char** source, const char* delim,
                         uint64_t* value) {
  return SplitOneUint64Token(source, delim, value);
}

// ----------------------------------------------------------------------
// SplitOneDecimalIntToken()
// SplitOneDecimalInt32Token()
// SplitOneDecimalUint32Token()
// SplitOneDecimalInt64Token()
// SplitOneDecimalUint64Token()
// Parse a single "delim"-delimited number from "*source" into "*value".
// Unlike SplitOneIntToken, etc., this function always interprets
// the numbers as decimal.
bool SplitOneDecimalIntToken(const char** source, const char* delim,
                             int* value);
bool SplitOneDecimalInt32Token(const char** source, const char* delim,
                               int32_t* value);
bool SplitOneDecimalUint32Token(const char** source, const char* delim,
                                uint32_t* value);
bool SplitOneDecimalInt64Token(const char** source, const char* delim,
                               int64_t* value);
bool SplitOneDecimalUint64Token(const char** source, const char* delim,
                                uint64_t* value);

// ----------------------------------------------------------------------
// SplitOneHexUint32Token()
// SplitOneHexUint64Token()
// Once more, for hexadecimal numbers (unsigned only).
bool SplitOneHexUint32Token(const char** source, const char* delim,
                            uint32_t* value);
bool SplitOneHexUint64Token(const char** source, const char* delim,
                            uint64_t* value);


#endif  // STRINGS_SPLIT_H_
