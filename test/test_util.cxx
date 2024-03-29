/*
 * Unit tests for src/util/
 */

#include "config.h"
#include "util/UriUtil.hxx"

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>

#include <string.h>
#include <stdlib.h>

class UriUtilTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(UriUtilTest);
	CPPUNIT_TEST(TestSuffix);
	CPPUNIT_TEST(TestRemoveAuth);
	CPPUNIT_TEST_SUITE_END();

public:
	void TestSuffix() {
		CPPUNIT_ASSERT_EQUAL((const char *)nullptr,
				     uri_get_suffix("/foo/bar"));
		CPPUNIT_ASSERT_EQUAL((const char *)nullptr,
				     uri_get_suffix("/foo.jpg/bar"));
		CPPUNIT_ASSERT_EQUAL(0, strcmp(uri_get_suffix("/foo/bar.jpg"),
					       "jpg"));
		CPPUNIT_ASSERT_EQUAL(0, strcmp(uri_get_suffix("/foo.png/bar.jpg"),
					       "jpg"));
	}

	void TestRemoveAuth() {
		CPPUNIT_ASSERT_EQUAL(std::string(),
				     uri_remove_auth("http://www.example.com/"));
		CPPUNIT_ASSERT_EQUAL(std::string("http://www.example.com/"),
				     uri_remove_auth("http://foo:bar@www.example.com/"));
		CPPUNIT_ASSERT_EQUAL(std::string("http://www.example.com/"),
				     uri_remove_auth("http://foo@www.example.com/"));
		CPPUNIT_ASSERT_EQUAL(std::string(),
				     uri_remove_auth("http://www.example.com/f:oo@bar"));
	}
};

CPPUNIT_TEST_SUITE_REGISTRATION(UriUtilTest);

int
main(gcc_unused int argc, gcc_unused char **argv)
{
	CppUnit::TextUi::TestRunner runner;
	auto &registry = CppUnit::TestFactoryRegistry::getRegistry();
	runner.addTest(registry.makeTest());
	return runner.run() ? EXIT_SUCCESS : EXIT_FAILURE;
}
