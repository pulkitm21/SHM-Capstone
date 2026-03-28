import { Navigate, Outlet, useLocation } from "react-router-dom";
import useAuth from "./useAuth";

/*
  This route guard blocks access to authenticated sections of the app.
  It waits for the auth bootstrap to finish before making a redirect decision.
*/
export default function ProtectedRoute() {
  const { isAuthenticated, isLoading } = useAuth();
  const location = useLocation();

  /*
    While the auth provider is restoring session state, do not redirect yet.
    This prevents route flicker on page refresh.
  */
  if (isLoading) {
    return null;
  }

  /*
    Unauthenticated users are redirected to the login page.
    The current location is stored so future login redirect behavior
    can return the user to the page they originally requested.
  */
  if (!isAuthenticated) {
    return <Navigate to="/login" replace state={{ from: location }} />;
  }

  return <Outlet />;
}