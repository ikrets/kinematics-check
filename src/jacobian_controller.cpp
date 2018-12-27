#include <rl/plan/Particle.h>
#include "jacobian_controller.h"
#include <iostream>
#include <fstream>

JacobianController::Result::operator bool() const
{
  assert(outcomes.size());

  if (outcomes.size() != 1)
    return false;
  Outcome outcome = *outcomes.begin();
  return outcome == Result::Outcome::REACHED || outcome == Result::Outcome::ACCEPTABLE_COLLISION;
}

std::string JacobianController::Result::description() const
{
  auto describeOutcome = [](Outcome outcome) {
    switch (outcome)
    {
      case Result::Outcome::REACHED:
        return "reached the goal frame";
      case Result::Outcome::JOINT_LIMIT:
        return "violated the joint limit";
      case Result::Outcome::SINGULARITY:
        return "ended in the singularity";
      case Result::Outcome::STEPS_LIMIT:
        return "went over the steps limit";
      case Result::Outcome::ACCEPTABLE_COLLISION:
        return "ended on acceptable collision";
      case Result::Outcome::UNACCEPTABLE_COLLISION:
        return "ended on unacceptable collision";
      case Result::Outcome::UNSENSORIZED_COLLISION:
        return "ended on unsensorized collision";
      case Result::Outcome::MISSED_REQUIRED_COLLISIONS:
        return "missing required collisions";
      default:
        assert(false);
    }
  };

  std::stringstream ss;
  bool first = true;
  for (auto o : outcomes)
  {
    if (!first)
      ss << ", ";
    else
      first = false;

    ss << describeOutcome(o);
  }

  return ss.str();
}

JacobianController::Result&
JacobianController::Result::setSingleOutcome(JacobianController::Result::Outcome single_outcome)
{
  outcomes.clear();
  outcomes.insert(single_outcome);

  return *this;
}

JacobianController::JacobianController(std::shared_ptr<rl::kin::Kinematics> kinematics,
                                       std::shared_ptr<rl::sg::bullet::Scene> bullet_scene, double delta,
                                       boost::optional<Viewer*> viewer)
  : kinematics_(kinematics), bullet_scene_(bullet_scene), delta_(delta)
{
  noisy_model_.kin = kinematics_.get();
  noisy_model_.model = bullet_scene_->getModel(0);
  noisy_model_.scene = bullet_scene_.get();
  noisy_model_.motionError = new rl::math::Vector(static_cast<int>(kinematics->getDof()));
  noisy_model_.initialError = new rl::math::Vector(static_cast<int>(kinematics->getDof()));

  random_engine_.seed(time(nullptr));

  if (viewer)
  {
    QObject::connect(this, SIGNAL(applyFunctionToScene(std::function<void(rl::sg::Scene&)>)), *viewer,
                     SLOT(applyFunctionToScene(std::function<void(rl::sg::Scene&)>)));
    QObject::connect(this, SIGNAL(reset()), *viewer, SLOT(reset()));
    QObject::connect(this, SIGNAL(drawConfiguration(const rl::math::Vector&)), *viewer,
                     SLOT(drawConfiguration(const rl::math::Vector&)));
  }
}

JacobianController::Result JacobianController::go(const rl::math::Vector& initial_configuration,
                                                  const rl::math::Transform& to_pose,
                                                  const CollisionTypes& collision_types, const Settings& settings)
{
  using namespace rl::math;
  using rl::plan::BeliefState;
  using rl::plan::Particle;

  std::size_t maximum_steps = static_cast<std::size_t>(10 / delta_);
  *noisy_model_.motionError = settings.joints_std_error;
  *noisy_model_.initialError = settings.initial_std_error;

  std::vector<Particle> initial_particles(settings.number_of_particles);
  for (std::size_t i = 0; i < settings.number_of_particles; ++i)
  {
    initial_particles[i].config.resize(initial_configuration.size());
    noisy_model_.sampleInitialError(initial_particles[i].config);
    initial_particles[i].config += initial_configuration;
  }

  BeliefState current_belief(initial_particles, &noisy_model_);

  auto required_counter = collision_types.makeRequiredCollisionsCounter();

  emit reset();
  emit drawConfiguration(current_belief.configMean());

  Result result;
  result.mean_trajectory.push_back(current_belief.configMean());

  for (std::size_t i = 0; i < maximum_steps; ++i)
  {
    auto q_dot = calculateQDot(current_belief, to_pose, delta_);
    // TODO: result REACHED if there is no required collision
    if (q_dot.isZero())
      return result.setSingleOutcome(required_counter->allRequiredPresent() ?
                                         Result::Outcome::REACHED :
                                         Result::Outcome::MISSED_REQUIRED_COLLISIONS);

    auto& particles = current_belief.getParticles();
    std::vector<Particle> next_particles(particles.size());
    std::vector<std::pair<std::string, std::string>> collisions;
    for (std::size_t j = 0; j < particles.size(); ++j)
    {
      Vector noise(noisy_model_.getDof());
      noisy_model_.sampleMotionError(noise);

      auto added_noise = (q_dot.array().abs().sqrt() * noise.array()).matrix();
      next_particles[j].config = particles[j].config + q_dot + added_noise;
      if (!noisy_model_.isValid(next_particles[j].config))
        result.outcomes.insert(Result::Outcome::JOINT_LIMIT);

      noisy_model_.setPosition(next_particles[j].config);
      noisy_model_.updateFrames();
      noisy_model_.updateJacobian();
      noisy_model_.updateJacobianInverse();
      noisy_model_.isColliding();

      if (noisy_model_.getDof() > 3 && noisy_model_.getManipulabilityMeasure() < 1.0e-3)
        result.outcomes.insert(Result::Outcome::SINGULARITY);

      auto collision_pairs = noisy_model_.scene->getLastCollisions();
      std::transform(collision_pairs.begin(), collision_pairs.end(), std::back_inserter(collisions),
                     [this](decltype(collision_pairs)::value_type c) {
                       return std::make_pair(getPartName(c.first.first), getPartName(c.first.second));
                     });
    }

    auto previous_mean = current_belief.configMean();
    current_belief = BeliefState(next_particles, &noisy_model_);
    // TODO rewrite to remove copying
    result.final_belief = current_belief;
    result.mean_trajectory.push_back(current_belief.configMean());

    emit drawConfiguration(current_belief.configMean());
    auto collision_constraints_check = checkCollisionConstraints(collisions, collision_types, *required_counter);
    std::copy(collision_constraints_check.failures.begin(), collision_constraints_check.failures.end(),
              std::inserter(result.outcomes, result.outcomes.begin()));

    if (!result.outcomes.empty())
      return result;
    else if (collision_constraints_check.success_termination)
      return result.setSingleOutcome(Result::Outcome::ACCEPTABLE_COLLISION);
  }

  return result.setSingleOutcome(Result::Outcome::STEPS_LIMIT);
}

rl::math::Vector JacobianController::calculateQDot(const rl::plan::BeliefState& belief,
                                                   const rl::math::Transform& goal_pose, double delta)
{
  using namespace rl::math;
  using rl::math::transform::toDelta;

  // Update the model
  noisy_model_.setPosition(belief.configMean());
  noisy_model_.updateFrames();
  noisy_model_.updateJacobian();
  noisy_model_.updateJacobianInverse();

  // Compute the jacobian
  Transform ee_world = noisy_model_.forwardPosition();
  Vector6 tdot;
  transform::toDelta(ee_world, goal_pose, tdot);

  // Compute the velocity
  Vector qdot = Vector::Zero(kinematics_->getDof());
  noisy_model_.inverseVelocity(tdot, qdot);

  if (qdot.norm() < delta)
    qdot.setZero();
  else
  {
    qdot.normalize();
    qdot *= delta;
  }

  return qdot;
}

JacobianController::CollisionConstraintsCheck
JacobianController::checkCollisionConstraints(const CollisionPairs& collisions, const CollisionTypes& collision_types,
                                              RequiredCollisionsCounter& required_counter)
{
  CollisionConstraintsCheck check;
  bool terminating_collision_present = false;

  for (auto& shapes_in_contact : collisions)
  {
    auto collision_type = collision_types.getCollisionType(shapes_in_contact.first, shapes_in_contact.second);

    // if the collision pair is ignored, touching with an unsensorized part is not a failure
    if (!isSensorized(shapes_in_contact.first) && !collision_type.ignored)
      check.failures.insert(Result::Outcome::UNSENSORIZED_COLLISION);

    required_counter.countCollision(shapes_in_contact.first, shapes_in_contact.second);

    if (collision_type.prohibited)
      check.failures.insert(Result::Outcome::UNACCEPTABLE_COLLISION);
    if (collision_type.terminating)
      terminating_collision_present = true;
  }

  if (terminating_collision_present && check.failures.empty() && required_counter.allRequiredPresent())
    check.success_termination = true;

  return check;
}

// will be killed by the hybrid automatons code
std::string JacobianController::getPartName(const std::string& address)
{
  auto split_position = address.find("_");
  auto body_address_str = address.substr(0, split_position);
  auto shape_number_str = address.substr(split_position + 1);

  auto body_address = reinterpret_cast<rl::sg::Body*>(std::stol(body_address_str, nullptr, 16));
  std::size_t shape_number = std::stoul(shape_number_str);

  return body_address->getShape(shape_number)->getName();
}

bool JacobianController::isSensorized(const std::string& part_name) const
{
  return part_name.find("sensor") != std::string::npos;
}

JacobianController::Settings JacobianController::Settings::NoUncertainty(std::size_t dof, double delta)
{
  Settings s;
  s.joints_std_error = rl::math::Vector::Zero(static_cast<int>(dof));
  s.initial_std_error = rl::math::Vector::Zero(static_cast<int>(dof));
  s.delta = delta;
  s.number_of_particles = 1;

  return s;
}