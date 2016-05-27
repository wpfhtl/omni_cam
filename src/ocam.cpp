#include "omni_cam/ocam.h"

#include <fstream>

#include <glog/logging.h>

namespace omni_cam {

namespace  {
Eigen::Matrix2d distortionToAffineCorrection(const Eigen::Vector3d& distortion)
{
  Eigen::Matrix2d result;
  // The output of the calibration toolbox is row-major (Matlab), therefore we
  // need to transpose here to get col-major.
  result << 1.0, distortion(2), distortion(1), distortion(0);
  return result;
}
}  // namespace

OCam::OCam(
    const Eigen::Vector2i& image_size,
    const Eigen::Matrix<double, 5, 1>& polynomial,
    const Eigen::Vector2d& principal_point, const Eigen::Vector3d& distortion,
    const Eigen::Matrix<double, 12, 1>& inverse_polynomial)
: image_size_(image_size),
  polynomial_(polynomial), principal_point_(principal_point),
  inverse_polynomial_(inverse_polynomial),
  affine_correction_(distortionToAffineCorrection(distortion)),
  affine_correction_inverse_(affine_correction_.inverse())
{}

void OCam::backProject3(
    const Eigen::Ref<const Eigen::Vector2d>& keypoint,
    Eigen::Vector3d* out_bearing_vector) const
{
  CHECK_NOTNULL(out_bearing_vector);
  const Eigen::Vector2d rectified =
      affine_correction_inverse_ * (keypoint - principal_point_);
  const double rho = rectified.norm();

  out_bearing_vector->head<2>() = rectified;

  (*out_bearing_vector)(2) = polynomial_(4);
  (*out_bearing_vector)(2) = polynomial_(3) + (*out_bearing_vector)(2) * rho;
  (*out_bearing_vector)(2) = polynomial_(2) + (*out_bearing_vector)(2) * rho;
  (*out_bearing_vector)(2) = polynomial_(1) + (*out_bearing_vector)(2) * rho;
  (*out_bearing_vector)(2) = polynomial_(0) + (*out_bearing_vector)(2) * rho;
  (*out_bearing_vector)(2) = (-1.0) * (*out_bearing_vector)(2);

  out_bearing_vector->normalize();
}

void OCam::project3(
    const Eigen::Ref<const Eigen::Vector3d>& point_3d,
    Eigen::Vector2d* out_keypoint,
    Eigen::Matrix<double, 2, 3>* out_jacobian_point) const
{
  CHECK_NOTNULL(out_keypoint);

  const double x = point_3d[0];
  const double y = point_3d[1];
  const double z = -point_3d[2];
  const double xy_norm2 = std::pow(x, 2) + std::pow(y, 2);
  const double xy_norm = std::sqrt(xy_norm2);
  const double z_by_xy_norm = z / xy_norm;
  const double theta = std::atan(z_by_xy_norm);

  Eigen::Matrix<double, kInversePolynomialOrder, 1> theta_powers;
  theta_powers[0] = 1.0;
  for(int i=1; i<theta_powers.size(); ++i)
  {
    theta_powers[i] = theta_powers[i-1]*theta;
  }

  const double rho = inverse_polynomial_.dot(theta_powers);

  Eigen::Vector2d raw_uv;
  raw_uv(0) = x / xy_norm * rho;
  raw_uv(1) = y / xy_norm * rho;

  (*out_keypoint) = affine_correction_ * raw_uv + principal_point_;

  if(out_jacobian_point) {
    // rho w.r.t theta
    double drho_dtheta = 0;
    for(int i=1; i < kInversePolynomialOrder; i++)
    {
      drho_dtheta += i * inverse_polynomial_[i] * theta_powers[i-1];
    }
    // theta w.r.t x y z
    const double xyz_norm_sqr = xy_norm2 + std::pow(z,2);
    const double dtheta_dx = (-1.0*x*z_by_xy_norm) / (xyz_norm_sqr);
    const double dtheta_dy = (-1.0*y*z_by_xy_norm) / (xyz_norm_sqr);
    const double dtheta_dz = xy_norm / (xyz_norm_sqr);
    // rho w.r.t x y z
    const double drho_dx = drho_dtheta * dtheta_dx;
    const double drho_dy = drho_dtheta * dtheta_dy;
    const double drho_dz = drho_dtheta * dtheta_dz;
    // uv_raw w.r.t x y z
    const double xy_sqr = xy_norm2;
    double duraw_dx = (xy_norm-x*x/xy_norm)/xy_sqr*rho + drho_dx*x/xy_norm;
    double duraw_dy = (-1.0*x*y/xy_norm)/xy_sqr*rho + drho_dy*x/xy_norm;
    double duraw_dz = drho_dz*x/xy_norm;
    double dvraw_dx = (-1.0*x*y/xy_norm)/xy_sqr*rho + drho_dx*y/xy_norm;
    double dvraw_dy = (xy_norm-y*y/xy_norm)/xy_sqr*rho + drho_dy*y/xy_norm;
    double dvraw_dz = drho_dz*y/xy_norm;
    (*out_jacobian_point) << duraw_dx, duraw_dy, -duraw_dz,
        dvraw_dx, dvraw_dy, -dvraw_dz;

    // uv w.r.t x y z
    (*out_jacobian_point) = affine_correction_ * (*out_jacobian_point);
  }
}

void OCam::print(std::ostream& out) const
{
  out << "  Projection = Omni" << std::endl;
  out << "  Image size = " << image_size_.transpose() << std::endl;
  out << "  Polynomial = " << polynomial_.transpose() << std::endl;
  out << "  Principal point = " << principal_point_.transpose() << std::endl;
  out << "  Inverse polynomial = " << inverse_polynomial_.transpose()
      << std::endl;
  out << "  Affine correction = " << std::endl << affine_correction_
      << std::endl;
  out << "  Affine correction inverse = " << std::endl
      << affine_correction_inverse_ << std::endl;
}

Eigen::VectorXd OCam::getIntrinsicParameters() const
{
  Eigen::VectorXd intrinsics(7);
  for(int idx = 0; idx < 5; idx++)
  {
    intrinsics(idx) = polynomial_(idx);
  }

  intrinsics(5) = principal_point_(0);
  intrinsics(6) = principal_point_(1);

  return intrinsics;
}

Eigen::VectorXd OCam::getDistortionParameters() const
{
  Eigen::VectorXd distortion(4);
  distortion(0) = affine_correction_(0,0);
  distortion(1) = affine_correction_(0,1);
  distortion(2) = affine_correction_(1,0);
  distortion(3) = affine_correction_(1,1);
  return distortion;
}

OCamPtr OCam::loadOCam(const std::string& parameter_file)
{
  std::ifstream param_fs(parameter_file);

  if(!param_fs.is_open())
  {
    LOG(ERROR) << "Fail to open file " << parameter_file;
    return OCamPtr();
  }

  Eigen::Vector2i image_size;
  Eigen::Matrix<double, 5, 1> polynomial;
  Eigen::Vector2d principal_point;
  Eigen::Vector3d distortion;
  Eigen::Matrix<double, kInversePolynomialOrder, 1> inverse_polynomial;

  // TODO: use exception here

  param_fs >> image_size(0) >> image_size(1);
  CHECK(param_fs.good()) << "Reading image size fails.";
  param_fs >>
      polynomial(0) >> polynomial(1) >> polynomial(2) >> polynomial(3) >> polynomial(4);
  CHECK(param_fs.good()) << "Reading polynomial fails.";

  param_fs >> principal_point(0) >> principal_point(1);
  CHECK(param_fs.good()) << "Reading principle point fails.";

  param_fs >> distortion(0) >> distortion(1) >> distortion(2);
  CHECK(param_fs.good()) << "Reading distortion fails.";

  for(size_t i=0; i<kInversePolynomialOrder; i++)
  {
    param_fs >> inverse_polynomial(i);
  }
  CHECK(param_fs.good()) << "Reading inverse polynomial fails.";

  return std::make_shared<OCam>(
        image_size, polynomial, principal_point, distortion, inverse_polynomial);
}

}  // namespace omni_cam
