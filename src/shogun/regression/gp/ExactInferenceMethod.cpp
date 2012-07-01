/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Copyright (C) 2012 Jacob Walker
 *
 * Code adapted from Gaussian Process Machine Learning Toolbox
 * http://www.gaussianprocess.org/gpml/code/matlab/doc/
 *
 */
#include <shogun/lib/config.h>

#ifdef HAVE_LAPACK

#include <shogun/regression/gp/ExactInferenceMethod.h>
#include <shogun/regression/gp/GaussianLikelihood.h>
#include <shogun/mathematics/lapack.h>
#include <shogun/mathematics/Math.h>
#include <shogun/labels/RegressionLabels.h>
#include <shogun/kernel/GaussianKernel.h>


using namespace shogun;

CExactInferenceMethod::CExactInferenceMethod() : CInferenceMethod()
{
	update_train_kernel();
	update_chol();
	update_alpha();
	update_parameter_hash();
}

CExactInferenceMethod::CExactInferenceMethod(CKernel* kern, CDotFeatures* feat,
		CMeanFunction* m, CLabels* lab, CLikelihoodModel* mod) :
			CInferenceMethod(kern, feat, m, lab, mod)
{
	update_train_kernel();
	update_chol();
	update_alpha();
}

CExactInferenceMethod::~CExactInferenceMethod()
{
}

void CExactInferenceMethod::update_all()
{
	m_label_vector =
			((CRegressionLabels*) m_labels)->get_labels().clone();

	m_feature_matrix =
			m_features->get_computed_dot_feature_matrix();

	update_data_means();
	update_train_kernel();
	update_chol();
	update_alpha();
}

void CExactInferenceMethod::check_members()
{
	if (!m_labels)
		SG_ERROR("No labels set\n");

	if (m_labels->get_label_type() != LT_REGRESSION)
		SG_ERROR("Expected RegressionLabels\n");

	if (!m_features)
		SG_ERROR("No features set!\n");

  	if (!m_features->has_property(FP_DOT))
		SG_ERROR("Specified features are not of type CDotFeatures\n");

  	if (m_labels->get_num_labels() != m_features->get_num_vectors())
		SG_ERROR("Number of training vectors does not match number of labels\n");

	if (m_features->get_feature_class() != C_DENSE)
		SG_ERROR("Expected Simple Features\n");

	if (m_features->get_feature_type() != F_DREAL)
		SG_ERROR("Expected Real Features\n");

	if (!m_kernel)
		SG_ERROR( "No kernel assigned!\n");

	if (!m_mean)
		SG_ERROR( "No mean function assigned!\n");

	if (m_model->get_model_type() != LT_GAUSSIAN)
	{
		SG_ERROR("Exact Inference Method can only use " \
				"Gaussian Likelihood Function.\n");
	}
}

CMap<SGString<const char>, float64_t> CExactInferenceMethod::
	get_marginal_likelihood_derivatives()
{
	check_members();

	if(update_parameter_hash())
		update_all();

	//This will be the vector we return
	CMap<SGString<const char>, float64_t> gradient(
			3+m_mean->m_parameters->get_num_parameters(),
			3+m_mean->m_parameters->get_num_parameters());

	//Get the sigma variable from the likelihood model
	float64_t m_sigma = dynamic_cast<CGaussianLikelihood*>(m_model)->get_sigma();

	//Placeholder Matrix
	SGMatrix<float64_t> temp1(m_ktrtr.num_rows, m_ktrtr.num_cols);

	//Placeholder Matrix
	SGMatrix<float64_t> temp2(m_alpha.vlen, m_alpha.vlen);

	//Derivative Matrix
	SGMatrix<float64_t> Q(m_L.num_rows, m_L.num_cols);

	//Vector used to fill diagonal of Matrix.
	SGVector<float64_t> diagonal(temp1.num_rows);
	SGVector<float64_t> diagonal2(temp2.num_rows);

	diagonal.fill_vector(diagonal.vector, temp1.num_rows, 1.0);
	diagonal2.fill_vector(diagonal2.vector, temp2.num_rows, 0.0);

	temp1.create_diagonal_matrix(temp1.matrix, diagonal.vector, temp1.num_rows);
	Q.create_diagonal_matrix(Q.matrix, diagonal.vector, temp2.num_rows);
	temp2.create_diagonal_matrix(temp2.matrix, diagonal2.vector, temp2.num_rows);

	memcpy(temp1.matrix, m_L.matrix,
			m_L.num_cols*m_L.num_rows*sizeof(float64_t));

	//Solve (L) Q = Identity for Q.
	clapack_dpotrs(CblasColMajor, CblasUpper,
			temp1.num_rows, Q.num_cols, temp1.matrix, temp1.num_cols,
			  Q.matrix, Q.num_cols);

	//Calculate alpha*alpha'
	cblas_dger(CblasColMajor, m_alpha.vlen, m_alpha.vlen,
			1.0, m_alpha.vector, 1, m_alpha.vector, 1,
			temp2.matrix, m_alpha.vlen);

	temp1.create_diagonal_matrix(temp1.matrix, diagonal.vector, temp1.num_rows);

	//Subtracct alpha*alpha' from Q.
	cblas_dsymm(CblasColMajor, CblasLeft, CblasUpper,
			temp1.num_rows, temp1.num_cols, 1.0/(m_sigma*m_sigma),
			Q.matrix, temp1.num_cols,
			temp1.matrix, temp1.num_cols, -1.0,
			temp2.matrix, temp2.num_cols);

	memcpy(Q.matrix, temp2.matrix,
		temp2.num_cols*temp2.num_rows*sizeof(float64_t));

	SGMatrix<float64_t> deriv = m_kernel->get_parameter_gradient("width");

	float64_t sum = 0;
	for (int i = 0; i < Q.num_rows; i++)
	{
		for (int j = 0; j < Q.num_cols; j++)
			sum += Q(i,j)*deriv(i,j)*m_scale*m_scale;
	}

	sum /= 2.0;

	gradient.add(SGString<const char>("width", strlen("width"), true), sum);

	sum = 0;

	for (int i = 0; i < Q.num_rows; i++)
	{
		for (int j = 0; j < Q.num_cols; j++)
			sum += Q(i,j)*m_ktrtr(i,j)*m_scale*2.0;
	}

	sum /= 2.0;

	gradient.add(SGString<const char>("scale", strlen("scale"), true), sum);

	sum = m_sigma*Q.trace(Q.matrix, Q.num_rows, Q.num_cols);
	
	gradient.add(SGString<const char>("sigma", strlen((char*)"sigma"), true), sum);

	for (int i = 0; i < m_mean->m_parameters->get_num_parameters(); i++)
	{
		TParameter* param = m_mean->m_parameters->get_parameter(i);

		SGVector<float64_t> data_means = m_mean->get_parameter_derivative(
				m_feature_matrix, param->m_name);

		sum = data_means.dot(data_means.vector, m_alpha.vector, m_alpha.vlen);

		gradient.add(SGString<const char>(param->m_name,
				strlen(param->m_name), true), sum);
	}

	return gradient;

}

SGVector<float64_t> CExactInferenceMethod::get_diagonal_vector()
{
	if(update_parameter_hash())
		update_all();

	check_members();

	float64_t m_sigma =
			dynamic_cast<CGaussianLikelihood*>(m_model)->get_sigma();

	SGVector<float64_t> result =
			SGVector<float64_t>(m_features->get_num_vectors());

	result.fill_vector(result.vector, m_features->get_num_vectors(),
			1.0/m_sigma);

	return result;
}

float64_t CExactInferenceMethod::get_negative_marginal_likelihood()
{
	if(update_parameter_hash())
		update_all();

	float64_t result;

	result = m_label_vector.dot(m_label_vector.vector, m_alpha.vector,
			m_label_vector.vlen)/2.0;

	float64_t m_sigma =
			dynamic_cast<CGaussianLikelihood*>(m_model)->get_sigma();

	for (int i = 0; i < m_L.num_rows; i++)
		result += CMath::log(m_L(i,i));

	result += m_L.num_rows * CMath::log(2*CMath::PI*m_sigma*m_sigma)/2.0;

	return result;
}

SGVector<float64_t> CExactInferenceMethod::get_alpha()
{
	if(update_parameter_hash())
		update_all();

	SGVector<float64_t> result(m_alpha);
	return result;
}

SGMatrix<float64_t> CExactInferenceMethod::get_cholesky()
{
	if(update_parameter_hash())
		update_all();

	SGMatrix<float64_t> result(m_L);
	return result;
}

void CExactInferenceMethod::update_train_kernel()
{
	m_kernel->cleanup();

	m_kernel->init(m_features, m_features);

	//K(X, X)
	SGMatrix<float64_t> kernel_matrix = m_kernel->get_kernel_matrix();

	m_ktrtr=kernel_matrix.clone();
}


void CExactInferenceMethod::update_chol()
{
	check_members();

	float64_t m_sigma =
			dynamic_cast<CGaussianLikelihood*>(m_model)->get_sigma();

	//Placeholder Matrices
	SGMatrix<float64_t> temp1(m_ktrtr.num_rows,
			m_ktrtr.num_cols);

	m_kern_with_noise = SGMatrix<float64_t>(m_ktrtr.num_rows,
			m_ktrtr.num_cols);

	SGMatrix<float64_t> temp2(m_ktrtr.num_rows,
			m_ktrtr.num_cols);

	//Vector to fill matrix diagonals
	SGVector<float64_t> diagonal(temp1.num_rows);
	diagonal.fill_vector(diagonal.vector, temp1.num_rows, 1.0);

	temp1.create_diagonal_matrix(temp1.matrix, diagonal.vector, temp1.num_rows);
	temp2.create_diagonal_matrix(temp2.matrix, diagonal.vector, temp2.num_rows);

	//Calculate first (K(X, X)+sigma*I)
	cblas_dsymm(CblasColMajor, CblasLeft, CblasUpper,
			m_ktrtr.num_rows, temp2.num_cols, (m_scale*m_scale)/(m_sigma*m_sigma),
			m_ktrtr.matrix, m_ktrtr.num_cols,
		temp2.matrix, temp2.num_cols, 1.0,
		temp1.matrix, temp1.num_cols);

	memcpy(m_kern_with_noise.matrix, temp1.matrix,
		temp1.num_cols*temp1.num_rows*sizeof(float64_t));

	//Get Lower triangle cholesky decomposition of K(X, X)+sigma*I)
	clapack_dpotrf(CblasColMajor, CblasUpper,
		temp1.num_rows, temp1.matrix, temp1.num_cols);

	m_L = SGMatrix<float64_t>(temp1.num_rows, temp1.num_cols);

	/* lapack for some reason wants only to calculate the upper triangle
	 * and leave the lower triangle with junk data. Finishing the job
	 * by filling the lower triangle with zero entries.
	 */
	for (int i = 0; i < temp1.num_rows; i++)
	{
		for (int j = 0; j < temp1.num_cols; j++)
		{
			if (i > j)
				temp1(i,j) = 0;
		}
	}

	memcpy(m_L.matrix, temp1.matrix,
		temp1.num_cols*temp1.num_rows*sizeof(float64_t));
}

void CExactInferenceMethod::update_alpha()
{
	float64_t m_sigma =
			dynamic_cast<CGaussianLikelihood*>(m_model)->get_sigma();

	//Placeholder Matrices
	SGMatrix<float64_t> temp1(m_L.num_rows,
			m_L.num_cols);

	for (int i = 0; i < m_label_vector.vlen; i++)
		m_label_vector[i] = m_label_vector[i] - m_data_means[i];

	m_alpha = SGVector<float64_t>(m_label_vector.vlen);

	memcpy(temp1.matrix, m_L.matrix,
		m_L.num_cols*m_L.num_rows*sizeof(float64_t));

	memcpy(m_alpha.vector, m_label_vector.vector,
		m_label_vector.vlen*sizeof(float64_t));

	//Solve (K(X, X)+sigma*I) alpha = labels for alpha.
	clapack_dposv(CblasColMajor, CblasLower,
			m_kern_with_noise.num_cols, 1, m_kern_with_noise.matrix,
			m_kern_with_noise.num_cols, m_alpha.vector,
			m_kern_with_noise.num_cols);

	for (int i = 0; i < m_alpha.vlen; i++)
		m_alpha[i] = m_alpha[i]/(m_sigma*m_sigma);

}

#endif // HAVE_LAPACK
