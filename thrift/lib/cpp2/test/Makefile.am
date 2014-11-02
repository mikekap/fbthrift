#
# Common thrift code generation rules
#
THRIFT = $(top_builddir)/compiler/thrift1

check_LIBRARIES = libService.a

gen-cpp2/Service_constants.cpp: Service.thrift
	PYTHONPATH=$(PY_LOCAL_PATH) python -mthrift_compiler.main --gen cpp2 $<
	$(THRIFT) --gen cpp:templates,cob_style -r $<
gen-cpp2/Service_types.cpp: gen-cpp2/Service_constants.cpp
gen-cpp2/TestService.cpp: gen-cpp2/Service_constants.cpp

libService_a_SOURCES = gen-cpp2/Service_constants.cpp \
		       gen-cpp2/Service_types.cpp \
		       gen-cpp2/TestService.cpp \
                       gen-cpp/Service_reflection.cpp

libService_a_CPPFLAGS = $(AM_CPPFLAGS) $(LIBEVENT_CPPFLAGS) -I../../cpp -I$(top_builddir)/../../gperftools-2.0.99/src

check_PROGRAMS = ThriftServerTest

ThriftServerTest_SOURCES = ThriftServerTest.cpp
ThriftServerTest_LDADD = -L../ libService.a ../libsaslstubs.a ../libthriftcpp2.la ../../cpp/libthrift.la
ThriftServerTest_LDFLAGS = -lglog -lgflags -lgtest $(BOOST_LDFLAGS)
ThriftServerTest_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_builddir)/../../gperftools-2.0.99/src

TESTS = ThriftServerTest

clean-local:
	rm -rf gen-cpp2/Service*
	rm -rf InstalledCheck

gen-cpp2/InstalledCheck_types.cpp: InstalledCheck.thrift
	$(PY_RUN_ENV) $(PYTHON) -mthrift_compiler.main --gen cpp2 InstalledCheck.thrift

InstalledCheck: gen-cpp2/TestService.cpp
	$(CXX) $(CXXFLAGS) -o InstalledCheck $(LDFLAGS) -L@prefix@/lib $(CPPFLAGS) -I@prefix@/include InstalledCheck.cpp gen-cpp2/InstalledCheck_types.cpp gen-cpp2/TestService.cpp -lthriftcpp2 -lthrift -lfolly -lglog -lboost_system -lboost_thread -lpthread -lm

installcheck: gen-cpp2/InstalledCheck_types.cpp InstalledCheck
