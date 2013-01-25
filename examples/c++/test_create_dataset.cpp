/*
 * test_create_dataset.cpp
 *
 *  Created on: Sep 4, 2012
 *      Author: Michael S. Hansen (michael.hansen@nih.gov)
 */

#include <iostream>
#include "ismrmrd_hdf5.h"
#include "fftw3.h"
#include "ismrmrd.hxx"

//Utility function for appending different sizes of arrays, used for testing here
template <typename T, int size_x, int size_y> int appendImageArray(ISMRMRD::IsmrmrdDataset& d, const char* varname)
{
	T a[size_x*size_y];
	std::vector<unsigned int> dims(2,0);
	dims[0] = size_x;
	dims[1] = size_y;

	//Let's make a simple square (rectangle depending on dimensions)
	for (int y = 0; y < size_y; y++) {
		for (int x = 0; x < size_x; x++) {
			if ( (x > (size_x>>2)) && (x < (size_x-(size_x>>2))) &&
				 (y > (size_y>>3)) && (y < (size_y-(size_y>>3)))) {
				a[y*size_x + x] = 1.0;
			} else {
				a[y*size_x + x] = 0.0;
			}
		}
	}

	ISMRMRD::NDArrayContainer<T> tmp(dims,a);

	return d.appendArray(tmp, varname);
}

//Helper function for the FFTW library
template<typename TI, typename TO> void circshift(TO *out, const TI *in, int xdim, int ydim, int xshift, int yshift)
{
  for (int i =0; i < ydim; i++) {
    int ii = (i + yshift) % ydim;
    for (int j = 0; j < xdim; j++) {
      int jj = (j + xshift) % xdim;
      out[ii * xdim + jj] = in[i * xdim + j];
    }
  }
}

#define fftshift(out, in, x, y) circshift(out, in, x, y, (x/2), (y/2))

/* MAIN APPLICATION */
int main(int argc, char** argv)
{
	std::cout << "ISMRMRD Test Dataset Creation App" << std::endl;

	const unsigned int readout = 256;
	const unsigned int phase_encoding_lines = 128;

	ISMRMRD::IsmrmrdDataset d("testdata.h5","dataset");

	//Let's create the "original" image in the file for reference
	if (appendImageArray< std::complex<float>, readout, phase_encoding_lines >(d, "the_square") < 0) {
		std::cout << "Error adding image to dataset" << std::endl;
		return -1;
	}

	//Read it back from the file
	boost::shared_ptr< ISMRMRD::NDArrayContainer<std::complex<float> > > img_test =
			d.readArray< std::complex<float> >("the_square", 0);

	if (img_test.get() == 0) {
		std::cout << "Error reading image array from file" << std::endl;
		return -1;
	}

	std::cout << "Image Array dimensions: ";
	for (int di = 0; di < img_test->dimensions_.size(); di++) {
		std::cout << img_test->dimensions_[di] << " ";
	}
	std::cout << std::endl;

	//Let's FFT this image to k-space
	fftwf_complex* tmp = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*img_test->data_.size());

	if (!tmp) {
		std::cout << "Error allocating temporary storage for FFTW" << std::endl;
		return -1;
	}

	fftshift(reinterpret_cast<std::complex<float>*>(tmp),&img_test->data_[0],img_test->dimensions_[0],img_test->dimensions_[1]);

	//Create the FFTW plan
	fftwf_plan p = fftwf_plan_dft_2d(img_test->dimensions_[1], img_test->dimensions_[0], tmp,tmp, FFTW_FORWARD, FFTW_ESTIMATE);

	fftwf_execute(p);

	fftshift(&img_test->data_[0],reinterpret_cast<std::complex<float>*>(tmp),img_test->dimensions_[0],img_test->dimensions_[1]);

	//Clean up.
	fftwf_destroy_plan(p);
	fftwf_free(tmp);

	//Let keep the "original" k-space in the file for reference
	if (d.appendArray(*img_test,"the_square_k") < 0) {
		std::cout << "Error adding kspace to dataset" << std::endl;
		return -1;
	}

	//Let's append the data to the file
	ISMRMRD::Acquisition acq;

	acq.data_.resize(readout*2);
/*	if (!acq.data_) {
		std::cout << "Error allocating memory for the acquisition" << std::endl;
	}
*/
	for (unsigned int i = 0; i < phase_encoding_lines; i++) {
		acq.head_.flags = 0;
		//Set some flags
		if (i == 0) {
			acq.setFlag(ISMRMRD::FlagBit(ISMRMRD::ACQ_FIRST_IN_SLICE));
		}
		if (i == (phase_encoding_lines-1)) {
			acq.setFlag(ISMRMRD::FlagBit(ISMRMRD::ACQ_LAST_IN_SLICE));
		}
		acq.head_.idx.kspace_encode_step_1 = i;
		acq.head_.active_channels = 1;
		acq.head_.available_channels = 1;
		acq.head_.number_of_samples = readout;
		acq.head_.center_sample = (readout>>1);
		acq.head_.sample_time_us = 5.0;
		memcpy(&acq.data_[0],&img_test->data_[i*readout],sizeof(float)*readout*2);
		d.appendAcquisition(&acq);
	}

	//Let's create a header, we will use the C++ class generated by XSD
	ISMRMRD::experimentalConditionsType exp(63500000); //~1.5T
	ISMRMRD::ismrmrdHeader h(exp);

	//Create an encoding section
	ISMRMRD::encodingSpaceType es(ISMRMRD::matrixSize(readout,phase_encoding_lines,1),ISMRMRD::fieldOfView_mm(600,300,6));
	ISMRMRD::encodingSpaceType rs(ISMRMRD::matrixSize((readout>>1),phase_encoding_lines,1),ISMRMRD::fieldOfView_mm(300,300,6));
	ISMRMRD::encodingLimitsType el;
	el.kspace_encoding_step_1(ISMRMRD::limitType(0,phase_encoding_lines-1,(phase_encoding_lines>>1)));
	ISMRMRD::encoding e(es,rs,el,ISMRMRD::trajectoryType::cartesian);

	//Add the encoding section to the header
	h.encoding().push_back(e);

	//Add any additional fields that you may want would go here....

	//e.g. parallel imaging
	//ISMRMRD::parallelImagingType parallel(ISMRMRD::accelerationFactorType(2,1));
	//parallel.calibrationMode(ISMRMRD::calibrationModeType::embedded);
    //h.parallelImaging(parallel);

	//Serialize the header
	xml_schema::namespace_infomap map;
	map[""].name = "http://www.ismrm.org/ISMRMRD";
	map[""].schema = "ismrmrd.xsd";
	std::stringstream str;
	ISMRMRD::ismrmrdHeader_(str, h, map);
	std::string xml_header = str.str();

	//Write the header to the data file.
	d.writeHeader(xml_header);

	return 0;
}
