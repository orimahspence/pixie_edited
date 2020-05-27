import DocsIcon from 'components/icons/docs';
import LogoutIcon from 'components/icons/logout';
import SettingsIcon from 'components/icons/settings';
import { DOMAIN_NAME } from 'containers/constants';
import gql from 'graphql-tag';
import * as React from 'react';
import { Link } from 'react-router-dom';

import { useQuery } from '@apollo/react-hooks';
import BaseAvatar from '@material-ui/core/Avatar';
import IconButton from '@material-ui/core/IconButton';
import ListItemIcon from '@material-ui/core/ListItemIcon';
import ListItemText from '@material-ui/core/ListItemText';
import Menu from '@material-ui/core/Menu';
import MenuItem from '@material-ui/core/MenuItem';
import { createStyles, makeStyles, Theme, withStyles } from '@material-ui/core/styles';

export const GET_USER_INFO = gql`
{
  user {
    email
    name
    picture
  }
}
`;

const useStyles = makeStyles((theme: Theme) => ({
  avatarSm: {
    backgroundColor: '#12D6D6',
  },
  avatarLg: {
    backgroundColor: '#12D6D6',
    width: theme.spacing(7),
    height: theme.spacing(7),
  },
  listItemText: {
    ...theme.typography.body2,
  },
  listItemHeader: {
    ...theme.typography.subtitle1,
    color: theme.palette.text.primary,
  },
  centeredListItemText: {
    textAlign: 'center',
  },
  centeredMenuItem: {
    flexDirection: 'column',
  },
}));

interface AvatarProps {
  name: string;
  picture?: string;
  className?: string;
}

const Avatar = (props: AvatarProps) => {
  // When the picture is an empty string, the fallback letter-style avatar of alt isn't used.
  // That only happens when the picture field is an invalid link.
  if (!props.picture && props.name.length > 0) {
    return <BaseAvatar className={props.className}>{props.name[0]}</BaseAvatar>;
  }
  return <BaseAvatar src={props.picture} alt={props.name} className={props.className} />;
};

const StyledListItemText = withStyles((theme: Theme) =>
  createStyles({
    primary: {
      ...theme.typography.body2,
      color: theme.palette.text.primary,
    },
  }),
)(ListItemText);

const StyledListItemIcon = withStyles(() =>
  createStyles({
    root: {
      minWidth: '30px',
    },
  }),
)(ListItemIcon);

const ProfileMenu = () => {
  const classes = useStyles();
  const [open, setOpen] = React.useState<boolean>(false);
  const [anchorEl, setAnchorEl] = React.useState(null);

  const openMenu = React.useCallback((event) => {
    setOpen(true);
    setAnchorEl(event.currentTarget);
  }, []);

  const closeMenu = React.useCallback(() => {
    setOpen(false);
    setAnchorEl(null);
  }, []);

  const { loading, error, data } = useQuery(GET_USER_INFO, { fetchPolicy: 'network-only' });

  if (loading || error || !data.user) {
    return null;
  }
  const user = data.user;
  return (
    <>
      <IconButton onClick={openMenu}>
        <Avatar name={user.name} picture={user.picture} className={classes.avatarSm} />
      </IconButton>
      <Menu open={open} onClose={closeMenu} anchorEl={anchorEl} getContentAnchorEl={null}
        anchorOrigin={{ vertical: 'bottom', horizontal: 'center' }}>
        <MenuItem key='profile' alignItems='center' button={false} className={classes.centeredMenuItem}>
          <Avatar name={user.name} picture={user.picture} className={classes.avatarLg} />
          <ListItemText primary={user.name} secondary={user.email}
            classes={{ primary: classes.listItemHeader, secondary: classes.listItemText }}
            className={classes.centeredListItemText} />
        </MenuItem>
        <MenuItem key='admin' button component={Link} to='/admin'>
          <StyledListItemIcon>
            <SettingsIcon />
          </StyledListItemIcon>
          <StyledListItemText primary='Admin' />
        </MenuItem>
        <MenuItem key='docs' button component='a' href={`https://docs.${DOMAIN_NAME}`} target='_blank'>
          <StyledListItemIcon>
            <DocsIcon />
          </StyledListItemIcon>
          <StyledListItemText primary='Documentation' />
        </MenuItem>
        <MenuItem key='logout' button component={Link} to='/logout'>
          <StyledListItemIcon>
            <LogoutIcon />
          </StyledListItemIcon>
          <StyledListItemText primary='Logout' />
        </MenuItem>
      </Menu>
    </>
  );
};

export default ProfileMenu;
