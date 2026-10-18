#ifndef _CMATERIALFILTER
#define _CMATERIALFILTER

#include "types.h"

#include "Collision/CMaterialList.hpp"

class CMaterialFilter {

public:
  static const CMaterialFilter skPassEverything;

  enum EFilterType {
    kFT_Always,
    kFT_Include,
    kFT_Exclude,
    kFT_IncludeExclude,
  };

  CMaterialFilter() : include(0x00000000FFFFFFFF), exclude(0), type(kFT_Always) {}
  CMaterialFilter(const CMaterialList& include, const CMaterialList& exclude, EFilterType type)
  : include(include), exclude(exclude), type(type) {}

  const CMaterialList& GetIncludeList() const { return include; }
  CMaterialList& IncludeList() { return include; }
  const CMaterialList& GetExcludeList() const { return exclude; }
  CMaterialList& ExcludeList() { return exclude; }
  // The mode itself: Remastered's collision filter stores it at +0x10 and its
  // EFilterType values are the source's modes (0 none, 1 include, 2 exclude,
  // 3 both), so a caller handing the filter to a translated-material gate needs
  // this rather than inferring it from the lists.
  EFilterType GetFilterType() const { return type; }
  static CMaterialFilter MakeInclude(const CMaterialList& include) {
    return CMaterialFilter(include, CMaterialList(), kFT_Include);
  }
  static CMaterialFilter MakeExclude(const CMaterialList& exclude) {
    return CMaterialFilter(CMaterialList(0x00000000FFFFFFFF), exclude, kFT_Exclude);
  }
  static CMaterialFilter MakeIncludeExclude(const CMaterialList& include,
                                            const CMaterialList& exclude) {
    return CMaterialFilter(include, exclude, kFT_IncludeExclude);
  }

  static const CMaterialFilter& GetPassEverything() { return skPassEverything; }

  bool Passes(const CMaterialList& other) const;

private:
  CMaterialList include;
  CMaterialList exclude;
  EFilterType type;
};
CHECK_SIZEOF(CMaterialFilter, 0x18)

#endif // _CMATERIALFILTER
