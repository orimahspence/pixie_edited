/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as QueryString from 'query-string';

import pixieAnalytics from 'app/utils/analytics';

export type AuthCallbackMode = 'cli_get' | 'cli_token' | 'ui';

export interface RedirectArgs {
  mode?: AuthCallbackMode;
  signup?: boolean;
  redirect_uri?: string;
  invite_token?: string;
}

export const getRedirectURL = (isSignup: boolean): string => {
  // Translate the login parameters to type of login flow.
  // local_mode && (no redirect_uri) -> cli_token
  // local_mode && redirect_uri -> cli_get
  // default: ui

  const redirectArgs: RedirectArgs = {
    mode: 'ui',
  };
  const parsed = QueryString.parse(window.location.search);
  if (parsed.redirect_uri && typeof parsed.redirect_uri === 'string') {
    redirectArgs.redirect_uri = String(parsed.redirect_uri);
  }

  if (parsed.local_mode && !!redirectArgs.redirect_uri) {
    redirectArgs.mode = 'cli_get';
  } else if (parsed.local_mode) {
    redirectArgs.mode = 'cli_token';
  }

  if (parsed.invite_token && typeof parsed.invite_token === 'string') {
    redirectArgs.invite_token = parsed.invite_token;
  }

  if (isSignup) {
    redirectArgs.signup = true;
  }

  if (parsed.tid && typeof parsed.tid === 'string') {
    pixieAnalytics.alias(parsed.tid);
  }
  return `${window.location.origin}/auth/callback?${QueryString.stringify(redirectArgs)}`;
};

// Takes in the window.location.search from a page and returns the params
// parsed into redirectArgs.
export const parseRedirectArgs = (params: QueryString.ParsedQuery): RedirectArgs => {
  const redirectArgs: RedirectArgs = {
    signup: !!params.signup,
    redirect_uri: params.redirect_uri && String(params.redirect_uri),
    invite_token: params.invite_token && String(params.invite_token),
    // Mode defaults to ui.
    mode: 'ui',
  };
  // We override the default only if the mode param is a valid member.
  if (['cli_get', 'cli_token', 'ui'].includes(params.mode as string)) {
    redirectArgs.mode = params.mode as AuthCallbackMode;
  }
  return redirectArgs;
};
