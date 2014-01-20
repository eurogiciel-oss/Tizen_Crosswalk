# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import features_utility
import svn_constants
from third_party.json_schema_compiler.json_parse import Parse


def _AddPlatformsFromDependencies(feature, features_bundle):
  features_map = {
    'api': features_bundle.GetAPIFeatures(),
    'manifest': features_bundle.GetManifestFeatures(),
    'permission': features_bundle.GetPermissionFeatures()
  }
  dependencies = feature.get('dependencies')
  if dependencies is None:
    return ['apps', 'extensions']
  platforms = set()
  for dependency in dependencies:
    dep_type, dep_name = dependency.split(':')
    dependency_features = features_map[dep_type]
    dependency_feature = dependency_features.get(dep_name)
    # If the dependency can't be resolved, it is inaccessible and therefore
    # so is this feature.
    if dependency_feature is None:
      return []
    platforms = platforms.union(dependency_feature['platforms'])
  feature['platforms'] = list(platforms)


class _FeaturesCache(object):
  def __init__(self, file_system, compiled_fs_factory, *json_paths):
    self._file_system = file_system
    self._cache = compiled_fs_factory.Create(
        file_system, self._CreateCache, type(self))
    self._json_path = json_paths[0]
    self._extra_paths = json_paths[1:]

  def _CreateCache(self, _, features_json):
    features = features_utility.Parse(Parse(features_json))
    for path in self._extra_paths:
      extra_json = self._file_system.ReadSingle(path).Get()
      features = features_utility.MergedWith(
          features_utility.Parse(Parse(extra_json)), features)
    return features

  def GetFeatures(self):
    if self._json_path is None:
      return {}
    return self._cache.GetFromFile(self._json_path).Get()


class FeaturesBundle(object):
  '''Provides access to properties of API, Manifest, and Permission features.
  '''
  def __init__(self, file_system, compiled_fs_factory, object_store_creator):
    self._api_cache = _FeaturesCache(
        file_system,
        compiled_fs_factory,
        svn_constants.API_FEATURES_PATH)
    self._manifest_cache = _FeaturesCache(
        file_system,
        compiled_fs_factory,
        svn_constants.MANIFEST_FEATURES_PATH,
        svn_constants.MANIFEST_JSON_PATH)
    self._permission_cache = _FeaturesCache(
        file_system,
        compiled_fs_factory,
        svn_constants.PERMISSION_FEATURES_PATH,
        svn_constants.PERMISSIONS_JSON_PATH)
    self._object_store = object_store_creator.Create(_FeaturesCache, 'features')

  def GetPermissionFeatures(self):
    return self._permission_cache.GetFeatures()

  def GetManifestFeatures(self):
    return self._manifest_cache.GetFeatures()

  def GetAPIFeatures(self):
    api_features = self._object_store.Get('api_features').Get()
    if api_features is None:
      api_features = self._api_cache.GetFeatures()
      # TODO(rockot): Handle inter-API dependencies more gracefully.
      # Not yet a problem because there is only one such case (windows -> tabs).
      # If we don't store this value before annotating platforms, inter-API
      # dependencies will lead to infinite recursion.
      self._object_store.Set('api_features', api_features)
      for feature in api_features.itervalues():
        _AddPlatformsFromDependencies(feature, self)
      self._object_store.Set('api_features', api_features)
    return api_features
