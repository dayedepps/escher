// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "examples/waterfall/scenes/uber_scene3.h"

#include "escher/geometry/tessellation.h"
#include "escher/geometry/types.h"
#include "escher/material/material.h"
#include "escher/renderer/image.h"
#include "escher/scene/model.h"
#include "escher/scene/stage.h"
#include "escher/shape/modifier_wobble.h"
#include "escher/util/stopwatch.h"
#include "escher/vk/vulkan_context.h"

using escher::vec2;
using escher::vec3;
using escher::MeshAttribute;
using escher::MeshSpec;
using escher::Object;
using escher::ShapeModifier;

UberScene3::UberScene3(escher::VulkanContext* vulkan_context,
                       escher::Escher* escher)
    : Scene(vulkan_context, escher) {}

void UberScene3::Init(escher::Stage* stage) {
  blue_ = ftl::MakeRefCounted<escher::Material>();
  red_ = ftl::MakeRefCounted<escher::Material>();
  pink_ = ftl::MakeRefCounted<escher::Material>();
  green_ = ftl::MakeRefCounted<escher::Material>();
  blue_green_ = ftl::MakeRefCounted<escher::Material>();
  purple_ = ftl::MakeRefCounted<escher::Material>();
  bg_ = ftl::MakeRefCounted<escher::Material>();
  mc1_ = ftl::MakeRefCounted<escher::Material>();
  mc2_ = ftl::MakeRefCounted<escher::Material>();

  blue_->set_color(vec3(0.188f, 0.188f, 0.788f));
  red_->set_color(vec3(0.98f, 0.15f, 0.15f));
  pink_->set_color(vec3(0.929f, 0.678f, 0.925f));
  green_->set_color(vec3(0.259f, 0.956f, 0.667));
  blue_green_->set_color(vec3(0.039f, 0.788f, 0.788f));
  purple_->set_color(vec3(0.588f, 0.239f, 0.729f));
  bg_->set_color(vec3(0.8f, 0.8f, 0.8f));

  mc1_->set_color(vec3(157.f / 255.f, 183.f / 255.f, 189.f / 255.f));
  mc2_->set_color(vec3(63.f / 255.f, 138.f / 255.f, 153.f / 255.f));
}

UberScene3::~UberScene3() {}

escher::Model* UberScene3::Update(const escher::Stopwatch& stopwatch,
                                  uint64_t frame_count,
                                  escher::Stage* stage) {
  float current_time_sec = stopwatch.GetElapsedSeconds();

  float screen_width = 1600.f;
  float screen_height = 1024.f;
  float min_height = 5.f;
  float max_height = 30.f;
  float elevation_range = max_height - min_height;

  std::vector<Object> objects;

  float PI = 3.14159265359f;
  float hex_circle_diameter = 170.f;
  float hex_circle_radius = hex_circle_diameter / 2.0f;
  float col_width = hex_circle_radius / tan(30.f * 180.f / PI);

  float num_rows_f = screen_height / hex_circle_radius;
  float num_cols_f = screen_width / col_width;

  int num_rows = num_rows_f;
  int num_cols = num_cols_f;

  float hex_current_x_pos = 0.f;
  float hex_current_y_pos = 0.f;
  float hex_x_offset = 0.f;

  float time_mult = 2.f;

  int circle_index = 0;
  int is_even = 0;

  for (int i = 0; i <= num_rows; i++) {
    hex_current_y_pos = i * hex_circle_diameter;
    if (fmod(i, 2) == 0) {
      is_even = 1;
      hex_x_offset = hex_circle_radius;
    } else {
      is_even = 0;
      hex_x_offset = 0.f;
    }

    for (int ii = 0; ii <= num_cols; ii++) {
      float time_offset = ii * 0.2f;
      float circle_elevation = 2.f;
      float circle_scale =
          (sin((current_time_sec + time_offset) * 1.25f) * .5f + .5f) * .5f +
          .5f;
      float circle_scale_alt =
          (cos((current_time_sec + (time_offset * 1.25f)) * 1.5f) * .5f + .5f) *
              .6f +
          .5f;

      hex_current_x_pos = ii * col_width + hex_x_offset;

      if (is_even == 1) {
        circle_elevation =
            sin(current_time_sec + time_offset * time_mult) * elevation_range +
            min_height + (elevation_range / 1.f);
      } else {
        circle_elevation =
            cos(current_time_sec + time_offset * time_mult) * elevation_range +
            min_height + (elevation_range / 1.f);
      }

      Object circle(Object::NewCircle(
          vec2(hex_current_x_pos, hex_current_y_pos),
          hex_circle_radius * circle_scale, circle_elevation, mc2_));
      objects.push_back(circle);

      Object circle_bg(Object::NewCircle(
          vec2(hex_current_x_pos, hex_current_y_pos),
          hex_circle_radius * circle_scale_alt, circle_elevation - 4.f, mc1_));
      objects.push_back(circle_bg);

      circle_index++;
    }
  }

  Object rectangle(Object::NewRect(
      vec2(0.f, 0.f), vec2(screen_width, screen_height), 1.f, bg_));

  objects.push_back(rectangle);

  // Create the Model
  model_ = std::unique_ptr<escher::Model>(new escher::Model(objects));
  model_->set_blur_plane_height(12.0f);
  model_->set_time(current_time_sec);

  return model_.get();
}