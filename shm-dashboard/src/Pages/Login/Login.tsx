import { useState } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import useAuth from "../../Auth/useAuth";
import "./Login.css";

type LoginRedirectState = {
  from?: {
    pathname?: string;
  };
};

function Login() {
  const [userId, setUserId] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");
  const [isSubmitting, setIsSubmitting] = useState(false);

  const navigate = useNavigate();
  const location = useLocation();
  const { login } = useAuth();

  /*
    If the user was redirected to login from a protected page,
    return them there after a successful sign-in. Otherwise,
    default back to the dashboard home page.
  */
  const redirectPath =
    (location.state as LoginRedirectState | null)?.from?.pathname || "/";

  async function handleSubmit(e: React.FormEvent) {
    e.preventDefault();
    setError("");

    /*
      Basic client-side validation prevents unnecessary auth calls
      when either field is empty.
    */
    if (!userId.trim() || !password.trim()) {
      setError("Please enter both UserID and password.");
      return;
    }

    setIsSubmitting(true);

    try {
      /*
        Auth is handled through the shared auth context so the login page
        does not manage session state directly.
      */
      await login({
        username: userId,
        password,
      });

      navigate(redirectPath, { replace: true });
    } catch (err) {
      /*
        The thrown error message is surfaced to the user when available.
        A generic fallback is used for unexpected failures.
      */
      const message =
        err instanceof Error ? err.message : "Unable to sign in.";

      setError(message);
    } finally {
      setIsSubmitting(false);
    }
  }

  return (
    <div className="loginPage">
      <div className="loginTile">
        <h1 className="loginTitle">Login</h1>

        <form onSubmit={handleSubmit} className="loginForm">
          <label className="loginLabel">
            UserID
            <input
              className="loginInput"
              type="text"
              value={userId}
              onChange={(e) => setUserId(e.target.value)}
              placeholder="UserID"
              autoComplete="username"
              disabled={isSubmitting}
            />
          </label>

          <label className="loginLabel">
            Password
            <input
              className="loginInput"
              type="password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              placeholder="******"
              autoComplete="current-password"
              disabled={isSubmitting}
            />
          </label>

          {error && <div className="loginError">{error}</div>}

          <button className="loginButton" type="submit" disabled={isSubmitting}>
            {isSubmitting ? "Signing In..." : "Sign In"}
          </button>
        </form>
      </div>
    </div>
  );
}

export default Login;