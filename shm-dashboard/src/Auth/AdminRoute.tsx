import { Navigate, Outlet, useLocation } from "react-router-dom";
import useAuth from "./useAuth";

/*
  This route guard is used for admin-only pages.
  Users must first be authenticated with admin role to access route.
*/
export default function AdminRoute() {
  const { isAuthenticated, isAdmin, isLoading } = useAuth();
  const location = useLocation();

  /*
    Wait until the auth bootstrap is complete before deciding whether
    the user should be allowed through this route.
  */
  if (isLoading) {
    return null;
  }

  /*
    If the user is not logged in, send them to the login page first.
    The original destination is preserved for later redirect handling.
  */
  if (!isAuthenticated) {
    return <Navigate to="/login" replace state={{ from: location }} />;
  }

  /*
    Logged-in non-admin users are redirected away from admin-only routes.
    Home is used as the safe fallback route for unauthorized access.
  */
  if (!isAdmin) {
    return <Navigate to="/" replace />;
  }

  return <Outlet />;
}