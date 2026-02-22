import { useState } from "react";
import { useNavigate } from "react-router-dom";
import "./Login.css";

function Login() {

  const [userId, setUserId] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");

  const navigate = useNavigate();


  function handleSubmit(e: React.FormEvent) {
    e.preventDefault();
    setError("");

    // Entry Validation - Currently just checks for empty fields
    if (!userId.trim() || !password.trim()) {
      setError("Please enter both UserID and password.");
      return;
    }

    // Add Backend Authentication Here, currently sets isAuth to true for any non-empty credentials
    sessionStorage.setItem("isAuth", "true");

    navigate("/", { replace: true });

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
                />
                </label>

                {error && <div className="loginError">{error}</div>}

                <button className="loginButton" type="submit">
                Sign In
                </button>
            </form>
            </div>
        </div>
);

}

export default Login;
