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

import AnnounceKit from 'announcekit-react';
import * as React from 'react';

import Drawer from '@material-ui/core/Drawer';
import {
  withStyles, Theme,
} from '@material-ui/core/styles';
import { createStyles } from '@material-ui/styles';

import HelpIcon from '@material-ui/icons/Help';
import List from '@material-ui/core/List';
import ListItem from '@material-ui/core/ListItem';
import ListItemIcon from '@material-ui/core/ListItemIcon';
import ListItemText from '@material-ui/core/ListItemText';
import AnnouncementIcon from '@material-ui/icons/Announcement';
import Menu from '@material-ui/icons/Menu';
import MenuItem from '@material-ui/core/MenuItem';
import Tooltip from '@material-ui/core/Tooltip';

import { Link } from 'react-router-dom';

import { ClusterContext } from 'common/cluster-context';
import UserContext from 'common/user-context';
import KeyboardIcon from '@material-ui/icons/Keyboard';
import {
  Avatar, ProfileMenuWrapper,
  ClusterIcon, CodeIcon, DocsIcon,
  LogoutIcon, NamespaceIcon, SettingsIcon,
  PixieLogo,
} from '@pixie-labs/components';
import { toEntityPathname, LiveViewPage } from 'containers/live-widgets/utils/live-view-params';
import {
  DOMAIN_NAME, ANNOUNCEMENT_ENABLED,
  ANNOUNCE_WIDGET_URL, CONTACT_ENABLED,
} from 'containers/constants';
import { Button } from '@material-ui/core';

const styles = (
  {
    spacing,
    palette,
    transitions,
    breakpoints,
  }: Theme) => createStyles({
  announcekit: {
    '& .announcekit-widget-badge': {
      position: 'absolute !important',
      top: spacing(2),
      left: spacing(4),
    },
  },
  docked: {
    position: 'absolute',
  },
  drawerClose: {
    borderRightWidth: spacing(0.2),
    borderRightStyle: 'solid',
    transition: transitions.create('width', {
      easing: transitions.easing.sharp,
      duration: transitions.duration.leavingScreen,
    }),
    width: spacing(8),
    zIndex: 1000,
    overflowX: 'hidden',
    paddingBottom: spacing(2),
    [breakpoints.down('sm')]: {
      display: 'none',
    },
  },
  compactHamburger: {
    display: 'none',
    [breakpoints.down('sm')]: {
      paddingTop: spacing(1),
      display: 'block',
    },
  },
  drawerOpen: {
    borderRightWidth: spacing(0.2),
    borderRightStyle: 'solid',
    width: spacing(29),
    zIndex: 1000,
    flexShrink: 0,
    whiteSpace: 'nowrap',
    transition: transitions.create('width', {
      easing: transitions.easing.sharp,
      duration: transitions.duration.enteringScreen,
    }),
    overflowX: 'hidden',
    paddingBottom: spacing(2),
  },
  expandedProfile: {
    flexDirection: 'column',
  },
  listIcon: {
    paddingLeft: spacing(2.5),
    paddingTop: spacing(1),
    paddingBottom: spacing(1),
  },
  pixieLogo: {
    fill: palette.primary.main,
    width: '48px',
  },
  profileIcon: {
    paddingLeft: spacing(1),
    paddingTop: spacing(1),
    paddingBottom: spacing(1),
  },
  profileText: {
    whiteSpace: 'nowrap',
    textOverflow: 'ellipsis',
    overflow: 'hidden',
    marginLeft: spacing(0.5),
  },
  sidebarToggle: {
    position: 'absolute',
    width: spacing(6),
    left: 0,
  },
  sidebarToggleSpacer: {
    width: spacing(6),
  },
  spacer: {
    flex: 1,
  },
  hideOnMobile: {
    // Same breakpoint (960px) at which the entire layout switches to suit mobile.
    [breakpoints.down('sm')]: {
      display: 'none',
    },
    width: '100%',
  },
});

const SideBarInternalLinkItem = ({
  classes, icon, link, text,
}) => (
  <Tooltip title={text}>
    <ListItem button component={Link} to={link} key={text} className={classes.listIcon}>
      <ListItemIcon>{icon}</ListItemIcon>
      <ListItemText primary={text} />
    </ListItem>
  </Tooltip>
);

const SideBarExternalLinkItem = ({
  classes, icon, link, text,
}) => (
  <Tooltip title={text}>
    <ListItem button component='a' href={link} key={text} className={classes.listIcon} target='_blank'>
      <ListItemIcon>{icon}</ListItemIcon>
      <ListItemText primary={text} />
    </ListItem>
  </Tooltip>
);

const HamburgerMenu = ({ classes, onToggle, logoLinkTo }) => (
  <ListItem button onClick={onToggle} key='Menu' className={classes.listIcon}>
    <ListItemIcon>
      <Menu />
    </ListItemIcon>
    <ListItemIcon>
      <Button
        component={Link}
        disabled={window.location.pathname.startsWith(logoLinkTo)}
        to={logoLinkTo}
        variant='text'
      >
        <PixieLogo className={classes.pixieLogo} />
      </Button>
    </ListItemIcon>
  </ListItem>
);

const SideBar = ({ classes, open, toggle }) => {
  const clusterContext = React.useContext(ClusterContext);
  const { user } = React.useContext(UserContext);

  const navItems = React.useMemo(() => {
    if (!clusterContext) {
      return [];
    }
    return [{
      icon: <ClusterIcon />,
      link: toEntityPathname({
        params: {},
        clusterName: clusterContext?.selectedClusterName,
        page: LiveViewPage.Cluster,
      }),
      text: 'Cluster',
    },
    {
      icon: <NamespaceIcon />,
      link: toEntityPathname({
        params: {},
        clusterName: clusterContext?.selectedClusterName,
        page: LiveViewPage.Namespaces,
      }),
      text: 'Namespaces',
    }];
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [clusterContext?.selectedClusterName]);

  return (
    <>
      <div className={classes.compactHamburger}>
        <ListItem button onClick={toggle} key='Menu' className={classes.listIcon}>
          <ListItemIcon>
            <Menu />
          </ListItemIcon>
        </ListItem>
      </div>
      <Drawer
        variant='permanent'
        className={open ? classes.drawerOpen : classes.drawerClose}
        classes={{
          paper: open ? classes.drawerOpen : classes.drawerClose,
          docked: classes.docked,
        }}
      >
        <List>
          <HamburgerMenu key='Menu' classes={classes} onToggle={toggle} logoLinkTo='/live' />
        </List>
        <List>
          {navItems.map(({ icon, link, text }) => (
            <SideBarInternalLinkItem key={text} classes={classes} icon={icon} link={link} text={text} />
          ))}
        </List>
        <div className={classes.spacer} />
        <List>
          <Tooltip title='Announcements'>
            <div className={classes.announcekit}>
              {
                ANNOUNCEMENT_ENABLED && (
                <AnnounceKit
                  widget={ANNOUNCE_WIDGET_URL}
                  user={
                        {
                          id: user.email,
                          email: user.email,
                        }
                 }
                  data={
                        {
                          org: user.orgName,
                        }
                   }
                >
                  <ListItem button key='announcements' className={classes.listIcon}>
                    <ListItemIcon><AnnouncementIcon /></ListItemIcon>
                    <ListItemText primary='Announcements' />
                  </ListItem>
                </AnnounceKit>
                )
              }
            </div>
          </Tooltip>
          <SideBarExternalLinkItem
            key='Docs'
            classes={classes}
            icon={<DocsIcon />}
            link={`https://docs.${DOMAIN_NAME}`}
            text='Docs'
          />
          {CONTACT_ENABLED && (
            <Tooltip title='Help'>
              <ListItem button id='intercom-trigger' className={classes.listIcon}>
                <ListItemIcon><HelpIcon /></ListItemIcon>
                <ListItemText primary='Help' />
              </ListItem>
            </Tooltip>
          )}
        </List>
      </Drawer>
    </>
  );
};

export default withStyles(styles)(SideBar);
