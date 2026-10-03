// Objective-C metadata reading (READ-ONLY). This is explicitly NOT an Objective-C runtime:
// nothing is registered, dispatched, messaged or called. It walks the metadata sections a
// compiler emits (class_ro_t, method/ivar/property lists, categories, protocols) so a caller
// can see which classes and selectors a binary carries before any runtime exists.
//
// Honesty rules:
//  - parse() does not throw for malformed metadata. Everything it cannot verify is reported in
//    Metadata::warnings and Metadata::complete is set to false.
//  - Both the classic (absolute pointer) and the modern (relative, 12/20-byte) encodings are
//    recognised; an unknown encoding is reported, never guessed at silently.
//  - Addresses are unslid vmaddrs, matching everything else in macho::Image.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mach_o/macho.h"

namespace radeki::objc {

struct Method {
  std::string selector;   // "doWork"
  std::string types;      // encoded type string, "" when unreadable
  uint64_t imp = 0;       // unslid vmaddr of the implementation
};

struct Ivar {
  std::string name, type;
  uint64_t offset = 0;    // value of the ivar offset slot, 0 when unreadable
  uint32_t size = 0;
};

struct Property {
  std::string name;
  std::string attrs;      // "T@\"NSString\",&,N"
};

struct Class {
  uint64_t addr = 0;           // unslid vmaddr of the objc_class object
  std::string name;            // "<unreadable>" when the class object could not be read
  std::string superName;       // "" for a root class
  uint64_t instanceSize = 0;
  std::vector<Method> methods;       // instance methods
  std::vector<Method> classMethods;  // read off the metaclass
  std::vector<Ivar> ivars;
  std::vector<Property> properties;
  std::vector<std::string> protocols;
};

struct Category {
  uint64_t addr = 0;
  std::string name;        // category name
  std::string className;   // class being extended, "" when unreadable
  std::vector<Method> methods, classMethods;
  std::vector<std::string> protocols;
};

struct Protocol {
  uint64_t addr = 0;
  std::string name;   // mangled name as stored in the image
  std::vector<Method> methods;
  std::vector<std::string> protocols;
};

struct Metadata {
  bool present = false;                // at least one __objc_* section exists
  bool complete = true;                // false when anything had to be skipped
  std::vector<std::string> sections;   // __objc_* sections found, in load order
  std::vector<Class> classes;          // __objc_classlist
  std::vector<Category> categories;    // __objc_catlist
  std::vector<Protocol> protocols;     // __objc_protolist
  std::vector<std::string> selectors;  // strings stored in __objc_methname
  std::vector<std::string> warnings;

  size_t methodCount() const;
  size_t selectorCount() const { return selectors.size(); }
};

// Never throws for malformed metadata; see the notes at the top of the file.
Metadata parse(const macho::Image& img);

}  // namespace radeki::objc
