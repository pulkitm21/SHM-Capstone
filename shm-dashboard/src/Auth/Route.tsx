import { Navigate, Outlet } from "react-router-dom";

function ProtectedRoute() {
  const authed = sessionStorage.getItem("isAuth") === "true";

  if (!authed) {
    return <Navigate to="/login" replace />;
  }

  return <Outlet />;
}

export default ProtectedRoute;
