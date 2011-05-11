/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <boost/test/unit_test.hpp>
#include <boost/cstdint.hpp>

#include <libpc/Iterator.hpp>
#include <libpc/Options.hpp>
#include <libpc/PointBuffer.hpp>
#include <libpc/SchemaLayout.hpp>
#include <libpc/drivers/qfit/Reader.hpp>
#include <libpc/filters/CacheFilter.hpp>
#include "Support.hpp"

#include <iostream>

using namespace libpc;

BOOST_AUTO_TEST_SUITE(QfitReaderTest)


#define Compare(x,y)    BOOST_CHECK_CLOSE(x,y,0.00001);


void Check_Point(const libpc::PointBuffer& data, const ::libpc::Schema& schema, 
                       std::size_t index, 
                       double xref, double yref, double zref,
                       boost::int32_t tref)
{

    int offsetX = schema.getDimensionIndex(libpc::Dimension::Field_X, libpc::Dimension::Int32);
    int offsetY = schema.getDimensionIndex(libpc::Dimension::Field_Y, libpc::Dimension::Int32);
    int offsetZ = schema.getDimensionIndex(libpc::Dimension::Field_Z, libpc::Dimension::Int32);
    int offsetTime = schema.getDimensionIndex(libpc::Dimension::Field_Time, libpc::Dimension::Int32);
    
    boost::int32_t x = data.getField<boost::int32_t>(index, offsetX);
    boost::int32_t y = data.getField<boost::int32_t>(index, offsetY);
    boost::int32_t z = data.getField<boost::int32_t>(index, offsetZ);
    boost::int32_t t = data.getField<boost::int32_t>(index, offsetTime);

    double x0 = schema.getDimension(offsetX).applyScaling<boost::int32_t>(x);
    double y0 = schema.getDimension(offsetY).applyScaling<boost::int32_t>(y);
    double z0 = schema.getDimension(offsetZ).applyScaling<boost::int32_t>(z);

  
    // std::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);
    // std::cout.precision(6);
    // std::cout << "expected x: " << xref << " y: " << yref << " z: " << zref << " t: " << tref << std::endl;
    // 
    // std::cout << "actual   x: " << x0 << " y: " << y0 << " z: " << z0 << " t: " << t << std::endl;
    // 
    Compare(x0, xref);
    Compare(y0, yref);
    Compare(z0, zref);
    BOOST_CHECK_EQUAL(t, tref);
}

BOOST_AUTO_TEST_CASE(test_10_word)
{
    libpc::Options options;
    // std::string filename = Support::datapath("20050903_231839.qi");

    std::string filename = Support::datapath("qfit/10-word.qi");

    
    boost::property_tree::ptree& tree = options.GetPTree();
    tree.put<std::string>("input", filename);
    libpc::drivers::qfit::Reader reader(options);
    BOOST_CHECK(reader.getDescription() == "QFIT Reader");
    BOOST_CHECK_EQUAL(reader.getName(), "drivers.qfit.reader");

    const Schema& schema = reader.getSchema();
    SchemaLayout layout(schema);

    PointBuffer data(layout, 3);
    
    libpc::SequentialIterator* iter = reader.createSequentialIterator();
    
    {
        boost::uint32_t numRead = iter->read(data);
        BOOST_CHECK_EQUAL(numRead,3);
    }

    delete iter;

    Check_Point(data, schema, 0, 59.205160, 221.826822, 32090.0, 0);
    Check_Point(data, schema, 1, 59.205161, 221.826740, 32019.0, 0);
    Check_Point(data, schema, 2, 59.205164, 221.826658, 32000.0, 0);


    return;
}

BOOST_AUTO_TEST_CASE(test_14_word)
{
    libpc::Options options;
    // std::string filename = Support::datapath("20050903_231839.qi");

    std::string filename = Support::datapath("qfit/14-word.qi");

    
    boost::property_tree::ptree& tree = options.GetPTree();
    tree.put<std::string>("input", filename);
    libpc::drivers::qfit::Reader reader(options);

    const Schema& schema = reader.getSchema();
    SchemaLayout layout(schema);

    PointBuffer data(layout, 3);
    
    libpc::SequentialIterator* iter = reader.createSequentialIterator();
    
    {
        boost::uint32_t numRead = iter->read(data);
        BOOST_CHECK_EQUAL(numRead,3);
    }

    delete iter;

    Check_Point(data, schema, 0, 35.623317, 244.306337, 1056830.000000, 903);
    Check_Point(data, schema, 1, 35.623280, 244.306260, 1056409.000000, 903);
    Check_Point(data, schema, 2, 35.623257, 244.306204, 1056483.000000, 903);


    return;
}



BOOST_AUTO_TEST_SUITE_END()
