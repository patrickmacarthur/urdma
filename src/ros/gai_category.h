/* \file gai_category.h
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#ifndef GAI_CATEGORY_H
#define GAI_CATEGORY_H

class gai_category_class : public std::error_category {
public:
	constexpr gai_category_class() noexcept : error_category() {}
	virtual const char *name() const noexcept { return "gai"; }
	virtual std::string message(int condition) const {
		return gai_strerror(condition);
	}
};

static inline const std::error_category &gai_category() noexcept
{
	static const gai_category_class singleton;
	return singleton;
}

#endif
